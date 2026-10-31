//
// Created by Hayden Rivas on 10/31/26.
//

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace mythril {
	struct SubresourceRange {
		uint32_t baseMip;
		uint32_t numMips;
		uint32_t baseLayer;
		uint32_t numLayers;

		bool contains(const SubresourceRange& o) const {
			return baseMip <= o.baseMip && baseMip + numMips >= o.baseMip + o.numMips &&
			       baseLayer <= o.baseLayer && baseLayer + numLayers >= o.baseLayer + o.numLayers;
		}
		bool overlaps(const SubresourceRange& o) const {
			const bool mipOverlap = !(baseMip + numMips <= o.baseMip || baseMip >= o.baseMip + o.numMips);
			const bool layerOverlap = !(baseLayer + numLayers <= o.baseLayer || baseLayer >= o.baseLayer + o.numLayers);
			return mipOverlap && layerOverlap;
		}
		bool operator==(const SubresourceRange& o) const = default;
	};

	// disjoint ranges covering every mip/layer of an image, each holding one subresource val
	template<typename T>
	class SubresourceMap {
	public:
		struct Entry {
			SubresourceRange range;
			T value;
		};

		SubresourceMap(uint32_t mips, uint32_t layers, const T& initial = {}) : _totalMips(mips), _totalLayers(layers) {
			_entries.push_back({wholeRange(), initial});
		}

		SubresourceRange wholeRange() const { return {0, _totalMips, 0, _totalLayers}; }

		// each returned range is clipped to the query
		std::vector<Entry> getOverlapping(const SubresourceRange& range) const {
			std::vector<Entry> overlapping;
			for (const Entry& entry: _entries) {
				if (!entry.range.overlaps(range)) continue;
				overlapping.push_back({intersect(entry.range, range), entry.value});
			}
			return overlapping;
		}

		void set(SubresourceRange range, const T& value) {
			if (range.numMips == 0 || range.numLayers == 0) {
				range = wholeRange();
			}
			if (range == wholeRange()) {
				_entries.clear();
				_entries.push_back({range, value});
				return;
			}
			std::vector<Entry> next;
			next.reserve(_entries.size() + 4);
			for (const Entry& entry: _entries) {
				if (!entry.range.overlaps(range)) {
					next.push_back(entry);
					continue;
				}
				appendDifference(next, entry, range);
			}
			next.push_back({range, value});
			_entries = std::move(next);
		}

	private:
		uint32_t _totalMips;
		uint32_t _totalLayers;
		std::vector<Entry> _entries;

		static SubresourceRange intersect(const SubresourceRange& a, const SubresourceRange& b) {
			const uint32_t mipBegin = std::max(a.baseMip, b.baseMip);
			const uint32_t mipEnd = std::min(a.baseMip + a.numMips, b.baseMip + b.numMips);
			const uint32_t layerBegin = std::max(a.baseLayer, b.baseLayer);
			const uint32_t layerEnd = std::min(a.baseLayer + a.numLayers, b.baseLayer + b.numLayers);
			return {mipBegin, mipEnd - mipBegin, layerBegin, layerEnd - layerBegin};
		}
		static void appendIfValid(std::vector<Entry>& entries, const SubresourceRange& range, const T& value) {
			if (range.numMips > 0 && range.numLayers > 0) {
				entries.push_back({range, value});
			}
		}
		// splits entry around removedRange: mips below/above the overlap, then layers below/above within the overlap's mips
		static void appendDifference(std::vector<Entry>& entries, const Entry& entry, const SubresourceRange& removedRange) {
			const SubresourceRange overlap = intersect(entry.range, removedRange);
			const uint32_t entryMipEnd = entry.range.baseMip + entry.range.numMips;
			const uint32_t overlapMipEnd = overlap.baseMip + overlap.numMips;
			const uint32_t entryLayerEnd = entry.range.baseLayer + entry.range.numLayers;
			const uint32_t overlapLayerEnd = overlap.baseLayer + overlap.numLayers;

			appendIfValid(entries, {entry.range.baseMip, overlap.baseMip - entry.range.baseMip, entry.range.baseLayer, entry.range.numLayers}, entry.value);
			appendIfValid(entries, {overlapMipEnd, entryMipEnd - overlapMipEnd, entry.range.baseLayer, entry.range.numLayers}, entry.value);
			appendIfValid(entries, {overlap.baseMip, overlap.numMips, entry.range.baseLayer, overlap.baseLayer - entry.range.baseLayer}, entry.value);
			appendIfValid(entries, {overlap.baseMip, overlap.numMips, overlapLayerEnd, entryLayerEnd - overlapLayerEnd}, entry.value);
		}
	};
} // namespace mythril
