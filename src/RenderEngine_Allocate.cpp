#include "RenderEngine.hpp"
#include <RGL/Span.hpp>
#include <mutex>
#include <RGL/RGL.hpp>
#include <RGL/Buffer.hpp>
#include <RGL/Device.hpp>
#include <RGL/CommandBuffer.hpp>

namespace RavEngine {
	RenderEngine::MeshRange RenderEngine::AllocateMesh(std::span<VertexNormalUV> vertices, std::span<uint32_t> indices)
	{
        std::lock_guard mtx{allocationLock};
		auto const vertexBytes = std::as_bytes(vertices);
		auto const indexBytes = std::as_bytes( indices );

	
		/**
		Find a Range that can fit the current allocation. If one does not exist, returns -1 (aka uint max)
		*/
		auto findPlacement = [](uint32_t requestedSize, const allocation_freelist_t& freeList) {
			uint32_t bestRangeIndex = -1;
			for (uint32_t i = 0; i < freeList.size(); i++) {
				auto& nextRange = freeList[i];
				if (requestedSize <= freeList[bestRangeIndex].count) {
					bestRangeIndex = i;
					break;
				}
			}
			return bestRangeIndex;
		};

		auto getAllocationLocation = [&findPlacement](uint32_t& allocation, uint32_t size, const uint32_t& currentSize, const allocation_freelist_t& freeList, auto realloc_fn) {
			allocation = -1;
			do {
				allocation = findPlacement(size, freeList);
				if (allocation == -1) {
					// resize to fit
					realloc_fn(currentSize + size);
				}

			} while (allocation != -1);
		};

		// figure out where to put the new data, resizing the buffer as needed
		uint32_t vertexAllocation = -1, indexAllocation = -1;
		getAllocationLocation(vertexAllocation, vertexBytes.size(), currentVertexSize, vertexFreeList,[this](uint32_t newSize) {ReallocateVertexAllocationToSize(newSize); });
		getAllocationLocation(indexAllocation, indexBytes.size(), currentIndexSize, indexFreeList,  [this](uint32_t newSize) {ReallocateIndexAllocationToSize(newSize); });

		// now we have the location to place the vertex and index data in the buffer
		// these numbers are stable because, if the buffer resized, then the only place it could be stored is at the end.
		// if the new data fits, then the buffer was not resized, so the indices are stable.

		auto consumeRange = [](uint32_t allocation, uint32_t allocatedSize, allocation_freelist_t& freeList, allocation_allocatedlist_t& allocatedList) {
			auto& rangeToUpdate = freeList.at(allocation);
			// if it fits exactly, delete it
			if (rangeToUpdate.count == allocatedSize) {
				freeList.erase(freeList.begin() + allocation);
			}
			else {
				// otherwise, modify it to represent the new shrunken size
				rangeToUpdate.start += allocatedSize;
			}
			// mark as allocated (copy into)
			allocatedList.push_back(rangeToUpdate);
			return rangeToUpdate;
		};

		// mark the ranges as consumed
		auto vertexPlacement = consumeRange(vertexAllocation, vertexBytes.size(), vertexFreeList, vertexAllocatedList);
		auto indexPlacement = consumeRange(indexAllocation, indexBytes.size(), indexFreeList, indexAllocatedList);

		// upload buffer data
		sharedVertexBuffer->UpdateBufferData(
			{ vertexBytes.data(), vertexBytes.size() }, vertexPlacement.start
		);
		sharedIndexBuffer->UpdateBufferData(
			{ indexBytes.data(), indexBytes.size() }, indexPlacement.start
		);
		
		return {
			vertexPlacement, indexPlacement
		};
	}
	void RenderEngine::DeallocateMesh(const MeshRange& range)
	{
        std::lock_guard mtx{allocationLock};
		
		auto deallocateData = [](Range range, allocation_allocatedlist_t& allocatedList, allocation_freelist_t& freeList) {

			uint32_t foundRangeIndex = -1;
			Range foundRange;
			for (; foundRangeIndex < allocatedList.size(); foundRangeIndex++) {
				const auto& nextRange = allocatedList[foundRangeIndex];
				if (nextRange.start >= range.start && nextRange.count >= range.count) {
					foundRange = allocatedList[foundRangeIndex];
					allocatedList.erase(allocatedList.begin() + foundRangeIndex);
					break;
				}
			}

			// xxxxx------xxxxx --> ----------xxxxx --> ----------------

			//does this range border any other ranges? If so, don't push a new range onto the freelist, isntead merge the existing ranges
			bool overlapFound = false;
			for (auto& range : freeList) {
				// the found range overlaps with the preceding range
				if (range.start + range.count == foundRange.start) {
					range.count += foundRange.count;
					overlapFound = true;
				}
				// the found range overlaps with the suceeding range
				if (foundRange.start + foundRange.count == range.start) {
					range.start -= foundRange.count;
					overlapFound = true;
				}
			}
			if (!overlapFound) {
				freeList.push_back(range);
			}

		};

		deallocateData(range.vertRange, vertexAllocatedList, vertexFreeList);
		deallocateData(range.indexRange, indexAllocatedList, indexFreeList);
	}

	void RavEngine::RenderEngine::ReallocateVertexAllocationToSize(uint32_t newSize)
	{
		auto oldBuffer = sharedVertexBuffer;
		// trash old buffer
		gcBuffers.enqueue(oldBuffer);
		sharedVertexBuffer = device->CreateBuffer({
			newSize,
			{.VertexBuffer = true},
			sizeof(VertexNormalUV),
			RGL::BufferAccess::Private
			});
		currentVertexSize = newSize;

		//TODO: compaction pass goes here

		std::vector<Range*> sortList;
		sortList.reserve(vertexAllocatedList.size());
		for (auto& range : vertexAllocatedList) {
			sortList.push_back(&range);
		}

		std::sort(sortList.begin(), sortList.end(), [](Range* a, Range* b) {
			return b->start - a->start;
		});

		auto commandbuffer = mainCommandQueue->CreateCommandBuffer();
		auto fence = device->CreateFence({});
		commandbuffer->Begin();

		// fill holes and copy data
		{
			uint32_t offset = 0;
			for (auto ptr : sortList) {
				auto oldstart = ptr->start;
				// copy buffers from:oldBuffer, offset:oldstart, size: ptr->count, to:sharedVertexBuffer offset:offset, size: ptr->count
				ptr->start = offset;
				offset += ptr->count;
			}
		}
		// submit and wait
		commandbuffer->End();
		commandbuffer->Commit({fence});
		fence->Wait();
	}
	void RenderEngine::ReallocateIndexAllocationToSize(uint32_t newSize)
	{
		sharedIndexBuffer = device->CreateBuffer({
			newSize,
			{.IndexBuffer = true},
			sizeof(uint32_t),
			RGL::BufferAccess::Private
		});
		currentIndexSize = newSize;
	}
}