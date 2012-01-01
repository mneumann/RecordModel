require 'RecordModelExt'

class RecordModel

  class Builder

    attr_accessor :attrs, :accs

    def initialize
      @attrs = [] 
      @accs = {}
      yield self
    end

    def key(id, type, sz=nil)
      raise if @attrs.assoc(id)
      @attrs << [id, :key, type, sz]
    end

    def val(id, type, sz=nil)
      raise if @attrs.assoc(id)
      @attrs << [id, :val, type, sz]
    end

    def acc(id, type, *args)
      case type
      when :uint64_x2
        id0, id1, _ = *args
        raise if _
        @accs[id] = %[
          def #{id}() (self.#{id0} << 64) | self.#{id1} end
          def #{id}=(v)
            self.#{id0} = (v >> 64) & 0xFFFFFFFF_FFFFFFFF
            self.#{id1} = (v)       & 0xFFFFFFFF_FFFFFFFF
          end
        ]
      else
        raise
      end
    end

  end

  def self.define(&block)
    b = Builder.new(&block)

    offset = 0
    info = {} 

    a = {}
    a[:key] = []
    a[:val] = []

    [:key, :val].each do |k|
      b.attrs.each do |id, k_or_v, type, sz|
        if k_or_v == k
          desc = def_descr(offset, type, sz)
          offset += type_size(type, sz)
          a[k] << desc
          info[id] = [desc, k]
        end
      end
    end

    model = new(a[:key], a[:val])
    klass = model.to_class

    info.each do |id, v|
      desc = v[0]
      klass.class_eval "def #{id}() self[#{desc}] end" 
      klass.class_eval "def #{id}=(v) self[#{desc}] = v end" 
    end

    b.accs.each do |id, code|
      klass.class_eval code
    end

    info.freeze
    klass.const_set(:INFO, info)
    klass.class_eval "def self.__info() INFO end"
    klass.class_eval "def __info() INFO end"

    return klass
  end

  def self.def_descr(offset, type, sz=nil)
    (offset << 16) | TYPES[type] | type_size(type, sz)
  end

  def self.type_size(type, sz)
    if TYPES[type] & 0xFF == 0
      raise if sz.nil?
      return sz
    else
      raise if sz and TYPES[type] & 0xFF != sz
      return TYPES[type] & 0xFF
    end
  end

  TYPES = {
    :uint64 => 0x0008,
    :uint32 => 0x0004,
    :uint16 => 0x0002,
    :uint8 => 0x0001,
    :double => 0x0108,
    :hexstr => 0x0200
  }

end

class RecordModelInstanceArray
  alias old_initialize initialize

  attr_reader :model_klass

  def initialize(model_klass, n=16, expandable=true)
    @model_klass = model_klass
    old_initialize(model_klass, n, expandable)
  end

  alias old_each each
  def each(instance=nil, &block)
    old_each(instance || @model_klass.new, &block)
  end

  def inspect
    [self.class, to_a]
  end

  def to_a
    a = []
    each {|i| a << i.dup}
    a
  end
end

class RecordModelInstance
  include Comparable

  def initialize(hash=nil)
    super()
    hash.each {|k, v| self.send(:"#{k}=", v) } if hash
  end

  def to_hash
    h = {}
    __info().each_key {|id| h[id] = send(id)}
    h
  end

  def keys_to_hash
    h = {}
    __info().each {|id,v| t = v[1]; h[id] = send(id) if t == :key}
    h
  end

  def values_to_hash
    h = {}
    __info().each {|id,v| t = v[1]; h[id] = send(id) if t == :val}
    h
  end

  def inspect
    [self.class, keys_to_hash, values_to_hash]
  end

  def self.make_array(n, expandable=true)
    RecordModelInstanceArray.new(self, n, expandable)
  end

  def self.__info_keys
    __info().each do |id, v|
      yield id, v[0] if v[1] == :key
    end
  end

  def self.build_query(query={})
    from = new()
    to = new()

    used_keys = [] 

    __info_keys() {|id, desc|
      if query.has_key?(id)
        used_keys << id
        case (q = query[id])
        when Range 
          raise ArgumentError if q.exclude_end?
          from[desc] = q.first 
          to[desc] = q.last
        else
          from[desc] = to[desc] = q
        end
      else
        from.set_min_or_max(desc, true) # set min
        to.set_min_or_max(desc, false) # set max
      end
    }

    raise ArgumentError unless (query.keys - used_keys).empty?

    return from, to
  end

  def self.db_query(db, query={}, &block)
    from, to = build_query(query)
    item = new()
    db.query(from, to, item, &block)
  end

  def self.db_query_into(db, itemarr=nil, query={})
    from, to = build_query(query)
    item = new()
    itemarr ||= make_array(1024)
    if db.query_into(from, to, item, itemarr)
      return itemarr
    else
      raise "query_into failed"
    end
  end

  #
  # Example usage: def_parser_descr(:uid, :campaign_id, nil, [:timestamp, :fixint, 3])
  #
  def self.def_parse_descr(*args)
    args.map {|arg|
      case arg 
      when nil
        0 # skip
      when Symbol
        __info[arg].first
      when Array
        id, type, extra = *arg
        if type == :fixint
          (((extra << 8) | 0x01) << 32) | __info[id].first
        else
          raise ArgumentError
        end
      else
        raise ArgumentError
      end
    }
  end

end

class RecordModel::LineParser
  require 'thread'

  def initialize(db, item_class, array_item_class, line_parse_descr)  
    @db = db
    @item_class = item_class
    @array_item_class = array_item_class
    @line_parse_descr = line_parse_descr
  end

  def convert_item(error, item)
    raise if error and error != -1
    return item
  end

  def import(io, array_sz=2**22, report_failures=false, report_progress_every=1_000_000, &block)
    line_parse_descr = @line_parse_descr

    inq, outq = Queue.new, Queue.new
    thread = start_db_thread(inq, outq) 

    # two arrays so that the log line parser and DB insert can work in parallel
    2.times { outq << @array_item_class.make_array(array_sz, false) }

    item = @item_class.new

    arr = outq.pop
    lines_read = 0
    lines_ok = 0

    while line = io.gets
      lines_read += 1
      begin
        item.zero!
        error = item.parse_line(line, line_parse_descr)
        if new_item = convert_item(error, item)
          arr << new_item
          lines_ok += 1
        end
        if arr.full?
          inq << arr 
	  arr = outq.pop
        end
      rescue 
        if report_failures and block
	  block.call(:failure, [$!, line])
        end
      end # begin .. rescue
      if report_progress_every and (lines_read % report_progress_every) == 0 and block
        block.call(:progress, [lines_read, lines_ok])
      end
    end # while

    inq << arr
    inq << :end

    thread.join

    return lines_read, lines_ok
  end

  protected

  def store_packet(packet)
    begin
      @db.put_bulk(packet)
    rescue
      p $!
    end
  end

  def start_db_thread(inq, outq)
    Thread.new {
      loop do
        packet = inq.pop
        break if packet == :end
        store_packet(packet)
        packet.reset
        outq << packet
      end
    }
  end
end
