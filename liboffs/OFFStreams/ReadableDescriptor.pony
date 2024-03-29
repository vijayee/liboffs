use "../BlockCache"
use "Streams"
use "Buffer"
use "Exception"
use "collections"

actor ReadableDescriptor[B: BlockType] is ReadablePushStream[Tuple val]
  var _isDestroyed: Bool = false
  let _ori: ORI val
  let _blockSize: USize
  let _subscribers': Subscribers
  let _cutPoint: USize // maximum length of descriptor in bytes
  let _tupleCount: USize // total number of  tupples descriptor
  var _tupleCounter: USize = 0
  var _currentDescriptor: (Buffer | None) = None
  var _currentTuple: (Tuple iso | None) = None
  let _bc: BlockCache[B]
  let _offsetTuple: USize // tuple containing offset
  var _offsetRemainder: Buffer
  let _descriptorPad: USize
  var _isReadable: Bool = true
  var _isPiped: Bool = false
  var _pipeNotifiers': (Array[Notify tag] iso | None) = None

  new create(bc: BlockCache[B], ori: ORI val, descriptorPad: USize) =>
    _subscribers' = Subscribers(3)
    _ori = ori
    _blockSize = BlockSize[B]()
    _tupleCount = (_ori.finalByte/ _blockSize) + (if (_ori.finalByte % _blockSize) > 0 then 1 else 0 end)
    _cutPoint = ((_blockSize / descriptorPad)  * descriptorPad)
    _offsetTuple = (_ori.fileOffset / _blockSize) + (if (_ori.fileOffset % _blockSize) > 0 then 1 else 0 end)
    _descriptorPad = descriptorPad
    _bc = bc
    _offsetRemainder = Buffer(_ori.tupleSize * _descriptorPad)

  fun readable(): Bool =>
    _isReadable

  fun destroyed(): Bool =>
    _isDestroyed

  fun ref isPiped(): Bool =>
    _isPiped

  fun ref pipeNotifiers(): (Array[Notify tag] iso^ | None) =>
    _pipeNotifiers' = None

  fun ref subscribers() : Subscribers =>
    _subscribers'

  fun ref autoPush(): Bool =>
    true

  be pipe(stream: WriteablePushStream[Tuple val] tag) =>
    if destroyed() then
      notifyError(Exception("Stream has been destroyed"))
    else
      let pipeNotifiers': Array[Notify tag] iso = try
         pipeNotifiers() as Array[Notify tag] iso^
      else
        let pipeNotifiers'' = recover Array[Notify tag] end
        consume pipeNotifiers''
      end

      let pipedNotify: PipedNotify iso =  object iso is PipedNotify
        let _stream: ReadableDescriptor[B] tag = this
        fun ref apply() =>
          _stream.push()
      end
      let pipedNotify': PipedNotify tag = pipedNotify
      pipeNotifiers'.push(pipedNotify')
      stream.subscribe(consume pipedNotify)

      let errorNotify: ErrorNotify iso = object iso  is ErrorNotify
        let _stream: ReadableDescriptor[B] tag = this
        fun ref apply (ex: Exception) => _stream.destroy(ex)
      end
      let errorNotify': ErrorNotify tag = errorNotify
      pipeNotifiers'.push(errorNotify')
      stream.subscribe(consume errorNotify)

      let closeNotify: CloseNotify iso = object iso  is CloseNotify
        let _stream: ReadableDescriptor[B] tag = this
        fun ref apply () => _stream.close()
      end
      let closeNotify': CloseNotify tag = closeNotify
      pipeNotifiers'.push(closeNotify')
      stream.subscribe(consume closeNotify)

      _pipeNotifiers' = consume pipeNotifiers'
      stream.piped(this)
      _isPiped = true
      notifyPipe()
    end

  fun ref _moveToOffset(descriptor: Buffer): (Buffer val, Buffer) =>
    var descriptor': Buffer = if _offsetRemainder.size() > 0 then
      Buffer(descriptor.size() + _offsetRemainder.size()).>append(_offsetRemainder = Buffer(0)).>append(descriptor)
    else
      descriptor
    end

    if _offsetTuple > 0 then
      if (_tupleCounter < _offsetTuple) then
        if _offsetTuple > ((descriptor'.size() % (_descriptorPad * _ori.tupleSize)) + _tupleCounter) then
          _tupleCounter = (descriptor'.size() - _descriptorPad) / (_descriptorPad * _ori.tupleSize)
          let cut: USize = (descriptor'.size() - _descriptorPad) - ((descriptor'.size() - _descriptorPad) % (_descriptorPad * _ori.tupleSize))
          _offsetRemainder = descriptor'.slice(cut, descriptor'.size() - _descriptorPad)
          (CopyBufferRange(descriptor', (descriptor'.size() - _descriptorPad), descriptor'.size()), Buffer(0))
        else
          let cut: USize = ((_offsetTuple - _tupleCounter) *  _ori.tupleSize)  * _descriptorPad
          descriptor' = descriptor'.slice(cut)
          _tupleCounter = _tupleCounter + (cut / _ori.tupleSize)
          (CopyBufferRange(descriptor', 0, _descriptorPad), descriptor'.slice(_descriptorPad))
        end
      else
        (CopyBufferRange(descriptor', 0, _descriptorPad), descriptor'.slice(_descriptorPad))
      end
    else
      (CopyBufferRange(descriptor', 0, _descriptorPad), descriptor'.slice(_descriptorPad))
    end

  be _receiveDescriptorBlock(block: (Block[B] | Exception | BlockNotFound), cb: ({(Tuple val)} val | None) = None) =>
    match block
      | let err: Exception => destroy(err)
      | BlockNotFound =>  destroy(Exception("Descriptor Block Not Found"))
      | let block': Block[B] =>
        var currentDescriptor: Buffer = match _currentDescriptor
          | None =>
            var currentDescriptor': Buffer = block'.data.slice(0, _cutPoint)
            currentDescriptor' = block'.data.slice(0, _cutPoint)
            if block'.hash == _ori.descriptorHash then
              currentDescriptor' = currentDescriptor'.slice(_ori.descriptorOffset)
            end
            _currentDescriptor = currentDescriptor'
            currentDescriptor'
          | let currentDescriptor: Buffer =>
            currentDescriptor
        end
        while currentDescriptor.size() > 0  do
          var currentTuple: Tuple iso = try
            ((_currentTuple = None) as Tuple iso^ )
          else
            recover Tuple(_ori.tupleSize) end
          end

          var cursor: (Buffer val, Buffer) = _moveToOffset(currentDescriptor)
          var key: Buffer val = cursor._1
          currentDescriptor = cursor._2

          while currentTuple.size() < _ori.tupleSize do
            if currentDescriptor.size() <= 0 then
              _getDescriptor(key)
              _currentTuple = consume currentTuple
              _currentDescriptor = None
              return
            else
              try currentTuple.push(key)? else destroy(Exception("Tuple append failure")) end
              if currentTuple.size() == _ori.tupleSize then
                break
              else
                cursor = _moveToOffset(currentDescriptor)
                key = cursor._1
                currentDescriptor = cursor._2
              end
            end
          end
          match cb
            | None =>
              notifyData(consume currentTuple)
            | let cb': {(Tuple val)} val =>
              cb'(consume currentTuple)
          end
          _tupleCounter = _tupleCounter + 1
          if _tupleCounter >= _tupleCount then
            notifyComplete()
            close()
            return
          end
        end
    end

  be _getDescriptor(key: Buffer val) =>
    let cb' =  {(block: (Block[B] | Exception | BlockNotFound)) (rd: ReadableDescriptor[B] tag = this) =>
      rd._receiveDescriptorBlock(block)
    } val
    _bc.get(_ori.descriptorHash, cb')

  be push() =>
    if destroyed() then
      notifyError(Exception("Stream has been destroyed"))
    else
      match _currentDescriptor
        | None =>
          let cb' =  {(block: (Block[B] | Exception | BlockNotFound)) (rd: ReadableDescriptor[B] tag = this) =>
            rd._receiveDescriptorBlock(block)
          } val
          _bc.get(_ori.descriptorHash, cb')
      end
    end

  be read(cb: {(Tuple val)} val, size:(USize | None) = None) =>
    if destroyed() then
      notifyError(Exception("Stream has been destroyed"))
    else
      match _currentDescriptor
        | None =>
          let cb' =  {(block: (Block[B] | Exception | BlockNotFound)) (rd: ReadableDescriptor[B] tag = this) =>
            rd._receiveDescriptorBlock(block, cb)
          } val
          _bc.get(_ori.descriptorHash, cb')
      end
    end

  be destroy(message: (String | Exception)) =>
    if not destroyed() then
      match message
        | let message' : String =>
          notifyError(Exception(message'))
        | let message' : Exception =>
          notifyError(message')
      end
      _isDestroyed = true
      let subscribers': Subscribers = subscribers()
      subscribers'.clear()
    end
  be close() =>
    if not destroyed() then
      _isDestroyed = true
      notifyClose()
      let subscribers': Subscribers = subscribers()
      subscribers'.clear()
      _pipeNotifiers' = None
      _isPiped = false
    end
