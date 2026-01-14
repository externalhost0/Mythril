//
// Created by Hayden Rivas on 10/1/25.
//

#pragma once

#include "Invalids.h"

#include <cassert>
#include <vector>

namespace mythril {
	template<typename Type>
	class InternalObjectHandle final {
	public:
		InternalObjectHandle() = default;
		InternalObjectHandle(uint32_t index, uint32_t gen) : _index(index), _generation(gen) {};

		uint32_t index() const {
			return _index;
		}

		uint32_t gen() const {
			return _generation;
		}

		bool valid() const {
			return _generation != 0;
		}

		bool empty() const {
			return _generation == 0;
		}

		bool operator==(const InternalObjectHandle<Type>& other) const {
			return this->_index == other._index && this->_generation == other._generation;
		}

	private:
		uint32_t _index = 0;
		uint32_t _generation = 0;
	};


	using BufferHandle = InternalObjectHandle<struct BufferTag>;
	using TextureHandle = InternalObjectHandle<struct TextureTag>;
	using SamplerHandle = InternalObjectHandle<struct SamplerTag>;
	using ShaderHandle = InternalObjectHandle<struct ShaderTag>;
	using GraphicsPipelineHandle = InternalObjectHandle<struct GraphicsPipelineTag>;
	using ComputePipelineHandle = InternalObjectHandle<struct ComputePipelineTag>;


// https://github.com/corporateshark/lightweightvk/blob/87d12061688d6f69b8b2c2d9799f00143cb0ee01/lvk/Pool.h#L13
// lightweight vk pool and handles
	template<typename HandleType, typename ActualObject>
	class HandlePool {
	private:
		static constexpr uint32_t kListEndSentinel = Invalid<uint32_t>;

		struct PoolEntry {
			explicit PoolEntry(ActualObject &&obj) : _obj(std::move(obj)) {}

			ActualObject _obj = {};
			uint32_t _gen = 1;
			uint32_t _nextFree = kListEndSentinel;
		};

		uint32_t _freeListHead = kListEndSentinel;
		uint32_t _numObjects = 0;

		std::vector<PoolEntry> _objects;

		// FIXME: temp fix to _objects not being public
		friend class CTX;
		friend class CommandBuffer;
	public:
		HandleType create(ActualObject &&obj) {
			uint32_t idx;
			if (_freeListHead != kListEndSentinel) {
				idx = _freeListHead;
				_freeListHead = _objects[idx]._nextFree;
				_objects[idx]._obj = std::move(obj);
			} else {
				idx = static_cast<uint32_t>(_objects.size());
				_objects.emplace_back(std::move(obj));
			}
			_numObjects++;
			return HandleType(idx, _objects[idx]._gen);
		}

		void destroy(HandleType handle) {
			if (handle.empty()) return;

			const uint32_t index = handle.index();
			assert(index < _objects.size());
			assert(handle.gen() == _objects[index]._gen);

			_objects[index]._obj = ActualObject{};
			_objects[index]._gen++;
			_objects[index]._nextFree = _freeListHead;
			_freeListHead = index;
			_numObjects--;
		}

		ActualObject* get(HandleType handle) {
			if (handle.empty()) return nullptr;

			const uint32_t index = handle.index();
			if (index >= _objects.size()) return nullptr;
			if (handle.gen() != _objects[index]._gen) return nullptr;

			return &_objects[index]._obj;
		}

		const ActualObject* get(HandleType handle) const {
			if (handle.empty()) return nullptr;

			const uint32_t index = handle.index();
			if (index >= _objects.size()) return nullptr;
			if (handle.gen() != _objects[index]._gen) return nullptr;

			return &_objects[index]._obj;
		}

		HandleType getHandle(uint32_t index) const {
			if (index >= _objects.size()) return {};
			return HandleType(index, _objects[index]._gen);
		}

		HandleType findObject(const ActualObject *obj) {
			if (!obj) return {};
			for (uint32_t idx = 0; idx < _objects.size(); ++idx) {
				if (&_objects[idx]._obj == obj) {
					return HandleType(idx, _objects[idx]._gen);
				}
			}
			return {};
		}

		void clear() {
			_objects.clear();
			_freeListHead = kListEndSentinel;
			_numObjects = 0;
		}

		uint32_t numObjects() const { return _numObjects; }
	};
}

template<typename Type>
struct std::hash<mythril::InternalObjectHandle < Type>> {
	size_t operator()(const mythril::InternalObjectHandle <Type> &handle) const noexcept {
		// Simple combination of index and generation
		const uint64_t combined = (static_cast<uint64_t>(handle.index()) << 32) | handle.gen();
		return std::hash<uint64_t>()(combined);
	}
};
