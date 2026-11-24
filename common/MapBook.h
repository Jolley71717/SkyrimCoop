#pragma once

#include "Protocol.h"

#include <cstdint>
#include <map>
#include <vector>

// The map markers the session's players discovered, as the server keeps them (docs/DESIGN.md, "Map markers shared").
// Each game says the markers its player gains while in the session (kMapMarkers: on the map, and can be fast travelled
// to); the server keeps what each marker has gained, passes on only what is new (each marker at most twice: seen, then
// discovered), and gives a joiner the whole book. It never grows past MAX_BOOK markers. No game and no network here:
// tests/MapShareTests.cpp.
namespace MapBook
{
	inline constexpr std::size_t MAX_BOOK = 8192;

	class Book
	{
	public:
		// What a_markers add to the book (a marker new to it, or a flag it lacked), each with all the flags the book now
		// holds for it: to pass on. Nothing past MAX_BOOK markers is kept.
		std::vector<Protocol::MapMarker> Take(const std::vector<Protocol::MapMarker>& a_markers)
		{
			std::vector<Protocol::MapMarker> gained;
			for (const auto& marker : a_markers) {
				const auto flags = static_cast<std::uint8_t>(marker.flags & Protocol::kMarkerAllFlags);
				if (marker.ref == 0 || flags == 0) {
					continue;
				}
				auto it = book.find(marker.ref);
				if (it == book.end()) {
					if (book.size() >= MAX_BOOK) {
						++refused;
						continue;
					}
					it = book.emplace(marker.ref, std::uint8_t{ 0 }).first;
				}
				if ((flags & ~it->second) == 0) {
					continue;
				}
				it->second |= flags;
				gained.push_back({ marker.ref, it->second });
			}
			return gained;
		}

		// The whole book, in messages' worth (at most Protocol::MAX_MARKERS each), for a joiner.
		[[nodiscard]] std::vector<std::vector<Protocol::MapMarker>> All() const
		{
			std::vector<std::vector<Protocol::MapMarker>> out;
			for (const auto& [ref, flags] : book) {
				if (out.empty() || out.back().size() >= Protocol::MAX_MARKERS) {
					out.emplace_back();
				}
				out.back().push_back({ ref, flags });
			}
			return out;
		}

		[[nodiscard]] std::size_t Size() const { return book.size(); }
		[[nodiscard]] std::uint64_t Refused() const { return refused; }

	private:
		std::map<std::uint32_t, std::uint8_t> book;
		std::uint64_t                         refused = 0;
	};
}
