#pragma once

#include "MemHelp.hpp"

#include <vector>
#include <queue>
#include <stack>

class ObjectPool{
protected:
	struct FileHeader{
		uint64_t indexInfoSize = 0;
		uint64_t freeInfoSize = 0;
		uint64_t orderInfoSize = 0;
		uint64_t bufferSize = 0;
	};

	uint8_t* _buffer = nullptr;
	uint64_t _bufferSize = 0;

	uint64_t _chunkSize = 0;

	std::vector<MemHelp::Info> _indexLocations;
	std::vector<MemHelp::Size> _freeMemory;
	std::vector<MemHelp::Location> _orderedIndexes;

	std::stack<MemHelp::Info> _removedMemory;

	inline void _expandInfoBuffers(uint64_t to = 0);

	inline void _rebuildIndexOrder();

	inline void _returnMemory(MemHelp::Info memory, bool rebuildQueue = true);

	inline MemHelp::Info _findMemory(uint64_t size);

public:
	class Iterator{
	protected:
		ObjectPool* _pool = nullptr;
		uint8_t* _buffer = nullptr;

		MemHelp::Info _location = 0;
		uint64_t _i = 0;

	public:
		Iterator(){}
		Iterator(ObjectPool* pool, MemHelp::Info location);
		Iterator(const Iterator& other);

		virtual ~Iterator(){}

		inline void operator=(const Iterator& other);

		// Get memory that iterator points to, nullptr if invalid
		inline uint8_t* get();

		// Iterate to next location, or invalidate iter if at end
		inline void next();

		// Check if iterator is valid, begin should always be valid if pool not empty
		inline bool valid() const;

		// Size of memory that iterator points to, 0 if invalid
		inline uint64_t size() const;
	};

	friend class Iterator;

	// Chunk size is the minimum the pool will increase by when making new allocations
	inline ObjectPool(uint64_t chunkSize);

	// Free all memory
	virtual inline ~ObjectPool();

	// Returns index to new clear block of memory of size 
	inline uint64_t insert(uint64_t size);

	// Set new or existing index to block of memory
	inline void set(uint64_t index, uint64_t size, bool copy = false);

	// Returns byte pointer to block of memory belonging to index
	inline uint8_t* get(uint64_t index);

	// Returns if index is set or not
	inline bool has(uint64_t index) const;

	// Marks index as removed and queues block of memory to be freed
	inline void remove(uint64_t index);

	// Frees removed blocks of memory forre-use
	inline void freeRemoved(uint64_t limit = 0);

	// Potentially allocate memory to guerentee enough of size, or chunk size if 0
	inline void reserve(uint64_t minimum = 0);

	// Potentially de-allocate memory by size if smaller than chunk, or as much as possible if 0
	inline void shrink(uint64_t maximum = 0);

	// Clear everything, and reset all buffers
	inline void clear();

	// Set minimum size to allocate new memory
	inline void setChunkSize(uint64_t size);

	// Return iterator to first element in linear memory, or non-valid if empty
	inline Iterator begin();

	// Shrink, free removed, and save data and pool info to file
	inline void save(const char* fileName);

	// Load file, clear pool, and copy data from file into pool
	inline void load(const char* fileName);

	// Total size in bytes ( totalSize() == usedSize()  + gapSize() + topSize() )
	inline uint64_t totalSize() const;

	// Returns total used buffer size in bytes
	inline uint64_t usedSize() const;

	// Return amount of fragmented un-used buffer memory in bytes
	inline uint64_t gapSize() const;

	// Return amount of top un-used buffer memory in bytes
	inline uint64_t topSize() const;
};

inline void ObjectPool::_expandInfoBuffers(uint64_t to){
	if (!to)
		to = _indexLocations.size() + 1;

	if (_indexLocations.size() < to)
		_indexLocations.resize(to);
}

inline void ObjectPool::_rebuildIndexOrder(){
	_orderedIndexes.clear();

	for (uint64_t i = 0; i < _indexLocations.size(); i++){
		if (has(i))
			_orderedIndexes.push_back(_indexLocations[i]);
	}

	std::sort(_orderedIndexes.begin(), _orderedIndexes.end());
}

inline void ObjectPool::_returnMemory(MemHelp::Info memory, bool rebuildQueue){
	std::queue<MemHelp::Info> carryOver;

	for (const MemHelp::Info& freeLocation : _freeMemory){
		if (memory.adjacent(freeLocation))
			memory = memory.combine(freeLocation);
		else
			carryOver.push(freeLocation);
	}

	std::memset(_buffer + memory.start, 0, memory.size);

	_freeMemory.clear();
	_freeMemory.push_back(memory);

	while (!carryOver.empty()){
		_freeMemory.push_back(carryOver.front());
		carryOver.pop();
	}

	if (rebuildQueue)
		std::sort(_freeMemory.rbegin(), _freeMemory.rend());
}

template <typename T>
inline static int compare(const void* a, const void* b){
	return *(T*)a < *(T*)b;
}

inline MemHelp::Info ObjectPool::_findMemory(uint64_t size){
	reserve(size);

	std::stack<MemHelp::Info> larger;

	while (_freeMemory.size() && _freeMemory.begin()->size >= size){
		larger.push(*_freeMemory.begin());
		_freeMemory.erase(_freeMemory.begin());
	}

	MemHelp::Info top = larger.top();
	larger.pop();

	MemHelp::Info sized(top.start, size);
	MemHelp::Info remain = top.subtract(sized);

	if (remain)
		_freeMemory.push_back(remain);

	while (larger.size()){
		_freeMemory.push_back(larger.top());
		larger.pop();
	}

	std::sort(_freeMemory.rbegin(), _freeMemory.rend());
	//std::qsort(_freeMemory.data(), _freeMemory.size(), sizeof(MemHelp::Size), compare<MemHelp::Size>);

	return sized;
}

inline ObjectPool::ObjectPool(uint64_t chunkSize){
	if (!chunkSize)
		chunkSize = 1;

	_chunkSize = chunkSize;
}

inline ObjectPool::~ObjectPool(){
	std::free(_buffer);
}

inline uint64_t ObjectPool::insert(uint64_t size){
	if (!size)
		size = 1;

	uint64_t index = 0;

	for (uint64_t i = 0; i < _indexLocations.size(); i++){
		if (!_indexLocations[i])
			index = i;
	}

	if (!index)
		index = _indexLocations.size();

	set(index, size);

	return index;
}

inline void ObjectPool::set(uint64_t index, uint64_t size, bool copy){
	if (!size)
		return;

	_expandInfoBuffers(index + 1);

	if (_indexLocations[index].size == size)
		return;

	MemHelp::Info memory = _findMemory(size);

	MemHelp::Info old = _indexLocations[index];

	if (old){
		if (copy && old.size <= memory.size)
			std::memcpy(_buffer + memory.start, _buffer + old.start, old.size);

		_returnMemory(old);
	}

	_indexLocations[index] = memory;

	_rebuildIndexOrder();
}

inline uint8_t* ObjectPool::get(uint64_t index){
	if (!has(index))
		return nullptr;

	MemHelp::Info memory = _indexLocations[index];

	return _buffer + memory.start;
}


inline bool ObjectPool::has(uint64_t index) const{
	if (index < _indexLocations.size() && _indexLocations[index] != 0)
		return true;

	return false;
}

inline void ObjectPool::remove(uint64_t index){
	if (!has(index))
		return;

	_removedMemory.push(_indexLocations[index]);
	_indexLocations[index] = 0;
}

inline void ObjectPool::freeRemoved(uint64_t limit){
	if (!limit)
		limit = _removedMemory.size();

	uint64_t inserted = 0;

	while (inserted < limit){
		_returnMemory(_removedMemory.top(), false);

		_removedMemory.pop();
		inserted++;
	}

	std::sort(_freeMemory.rbegin(), _freeMemory.rend());

	_rebuildIndexOrder();
}

inline void ObjectPool::reserve(uint64_t minimum){
	MemHelp::Info top;
	
	if (_freeMemory.size() && _freeMemory.begin()->end() == _bufferSize)
		top = *_freeMemory.begin();

	if (top.size >= minimum)
		return;

	if (_chunkSize > minimum)
		minimum = _chunkSize;

	MemHelp::Info newTop(_bufferSize, minimum - top.size);

	_bufferSize += newTop.size;
	_buffer = MemHelp::allocate(_bufferSize, _buffer);

	_returnMemory(newTop);
}

inline void ObjectPool::shrink(uint64_t maximum){
	MemHelp::Info top = *_freeMemory.begin();

	if (top.end() != _bufferSize || top.size < _chunkSize)
		return;

	if (!maximum)
		maximum = top.size;

	_freeMemory.erase(_freeMemory.begin());
	_freeMemory.push_back(MemHelp::Info(top.start, top.size - maximum));

	_bufferSize -= maximum;
	_buffer = MemHelp::allocate(_bufferSize, _buffer);

	std::sort(_freeMemory.rbegin(), _freeMemory.rend());
}

inline void ObjectPool::clear(){
	std::free(_buffer);
	_buffer = nullptr;

	_bufferSize = 0;

	std::vector<MemHelp::Info>().swap(_indexLocations);
	std::vector<MemHelp::Size>().swap(_freeMemory);
	std::vector<MemHelp::Location>().swap(_orderedIndexes);

	_removedMemory = std::stack<MemHelp::Info>();
}

inline void ObjectPool::setChunkSize(uint64_t size){
	if (size)
		_chunkSize = size;
}

inline ObjectPool::Iterator ObjectPool::begin(){
	if (_orderedIndexes.empty())
		return Iterator();

	return Iterator(this, *_orderedIndexes.begin());
}

inline void ObjectPool::save(const char* fileName){
	freeRemoved();
	shrink();

	std::FILE* file = nullptr;

	file = std::fopen(fileName, "wb");
	std::fclose(file);

	file = std::fopen(fileName, "ab");

	FileHeader header;

	header.freeInfoSize = _freeMemory.size();
	header.indexInfoSize = _indexLocations.size();
	header.orderInfoSize = _orderedIndexes.size();
	header.bufferSize = _bufferSize;

	std::fwrite(&header, sizeof(FileHeader), 1, file);
	std::fwrite(_freeMemory.data(), sizeof(MemHelp::Size), _freeMemory.size(), file);
	std::fwrite(_indexLocations.data(), sizeof(MemHelp::Info), _indexLocations.size(), file);
	std::fwrite(_orderedIndexes.data(), sizeof(MemHelp::Location), _orderedIndexes.size(), file);
	std::fwrite(_buffer, 1, _bufferSize, file);

	std::fclose(file);
}

inline void ObjectPool::load(const char* fileName){
	std::FILE* file = nullptr;

	file = std::fopen(fileName, "rb");

	if (!file)
		return;

	clear();

	FileHeader header;

	std::fread(&header, sizeof(FileHeader), 1, file);
	_freeMemory.reserve(header.freeInfoSize);
	_indexLocations.reserve(header.indexInfoSize);
	_orderedIndexes.reserve(header.orderInfoSize);
	_buffer = MemHelp::allocate(header.bufferSize, _buffer);
	_bufferSize = header.bufferSize;

	std::fread(_freeMemory.data(), sizeof(MemHelp::Size), _freeMemory.size(), file);
	std::fread(_indexLocations.data(), sizeof(MemHelp::Info), _indexLocations.size(), file);
	std::fread(_orderedIndexes.data(), sizeof(MemHelp::Location), _orderedIndexes.size(), file);
	std::fread(_buffer, 1, _bufferSize, file);

	std::fclose(file);
}

inline uint64_t ObjectPool::totalSize() const{
	return _bufferSize;
}

inline uint64_t ObjectPool::usedSize() const{
	uint64_t used = 0;

	for (const MemHelp::Info& block : _indexLocations){
		used += block.size;
	}

	return used;
}

inline uint64_t ObjectPool::gapSize() const{
	uint64_t empty = 0;

	for (const MemHelp::Info& free : _freeMemory){
		if (free.end() != _bufferSize)
			empty += free.size;
	}

	return empty;
}

inline uint64_t ObjectPool::topSize() const{
	if (_freeMemory.size() && _freeMemory.begin()->end() == _bufferSize)
		return _freeMemory.begin()->size;

	return 0;
}

inline ObjectPool::Iterator::Iterator(ObjectPool* pool, MemHelp::Info location){
	if (!pool || !pool->_buffer)
		return;

	_pool = pool;
	_buffer = pool->_buffer;
	_location = location;
	_i = 0;
}

inline ObjectPool::Iterator::Iterator(const Iterator & other){
	operator=(other);
}

inline void ObjectPool::Iterator::operator=(const Iterator& other){
	_buffer = other._buffer;
	_location = other._location;
}

inline uint8_t * ObjectPool::Iterator::get(){
	if (valid())
		return _buffer + _location.start;

	return nullptr;
}

inline void ObjectPool::Iterator::next(){
	if (!valid())
		return;

	_i++;

	if (_i < _pool->_orderedIndexes.size())
		_location = _pool->_orderedIndexes[_i];
	else
		_location = 0;
}

inline bool ObjectPool::Iterator::valid() const{
	return _pool && _buffer && _location;
}

inline uint64_t ObjectPool::Iterator::size() const{
	if (valid())
		return _location.size;

	return 0;
}