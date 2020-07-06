#pragma once
/*
* Copyright 2016 Nu-book Inc.
* Copyright 2016 ZXing authors
* Copyright 2020 Axel Waggershauser
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "BitArray.h"
#include "DecodeStatus.h"
#include "Pattern.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <memory>
#include <type_traits>

/*
Code39 : 1:2/3, 5+4+1 (0x3|2x1 wide) -> 12-15 mods, v1-? | ToNarrowWide(OMG 1) == *
Codabar: 1:2/3, 4+3+1 (1x1|1x2|3x0 wide) -> 9-13 mods, v1-? | ToNarrowWide(OMG 2) == ABCD
ITF    : 1:2/3, 5+5   (2x2 wide) -> mods, v6-?| .5, .38 == * | qz:10

Code93 : 1-4, 3+3 -> 9 mods  v1-? | round to 1-4 == *
Code128: 1-4, 3+3 -> 11 mods v1-? | .7, .25 == ABC | qz:10
UPC/EAN: 1-4, 2+2 -> 7 mods  f    | .7, .48 == *
  UPC-A: 11d 95m = 3 + 6*4 + 5 + 6*4 + 3 = 59
  EAN-13: 12d 95m

RSS14  : 1-8, finder: (15,2+3), symbol: (15/16,4+4) | .45, .2 (finder only), 14d
  code = 2xguard + 2xfinder + 4xsymbol = (96,23), stacked = 2x50 mods
RSSExp.:  v?-74d/?-41c
*/

namespace ZXing {

class Result;

namespace OneD {

/**
* Encapsulates functionality and implementation that is common to all families
* of one-dimensional barcodes.
*
* @author dswitkin@google.com (Daniel Switkin)
* @author Sean Owen
*/
class RowReader
{
public:

	struct DecodingState
	{
		virtual ~DecodingState() = default;
	};

	Result decodeSingleRow(int rowNumber, const BitArray& row) const;

	virtual ~RowReader() {}

	/**
	* <p>Attempts to decode a one-dimensional barcode format given a single row of
	* an image.</p>
	*
	* @param rowNumber row number from top of the row
	* @param row the black/white pixel data of the row
	* @param hints decode hints
	* @return {@link Result} containing encoded string and start/end of barcode
	* @throws NotFoundException if no potential barcode is found
	* @throws ChecksumException if a potential barcode is found but does not pass its checksum
	* @throws FormatException if a potential barcode is found but format is invalid
	*/
	virtual Result decodeRow(int rowNumber, const BitArray& row, std::unique_ptr<DecodingState>& state) const = 0;

	virtual Result decodePattern(int rowNumber, const PatternView& row, std::unique_ptr<DecodingState>& state) const;

	/**
	* Scans the given bit range for a pattern identified by evaluating the function object match for each
	* successive run of counters.size() bars. If the pattern is found, it returns the bit range with the
	* pattern. Otherwise an empty range is returned.
	*
	* @param begin/end bit range to scan for pattern
	* @param counters array into which to record counts
	* @param match predicate that gets evaluated to identify the pattern
	* @throws NotFoundException if counters cannot be filled entirely from row before running out
	*  of pixels
	*/
	template <typename Iterator, typename Container, typename Predicate>
	static Range<Iterator> FindPattern(Iterator begin, Iterator end, Container& counters, Predicate match) {
		if (begin == end)
			return {end, end};

		Iterator li = begin, i = begin;
		auto currentCounter = counters.begin();
		typedef typename std::decay<decltype(*currentCounter)>::type CounterValue;
		while ((i = BitArray::getNextSetTo(i, end, !*i)) != end) {
			*currentCounter = static_cast<CounterValue>(i - li);
			if (++currentCounter == counters.end()) {
				if (match(begin, i, counters)) {
					return {begin, i};
				}
				std::advance(begin, counters[0] + counters[1]);
				std::copy(counters.begin() + 2, counters.end(), counters.begin());
				std::advance(currentCounter, -2);
			}
			li = i;
		}
		// if we ran into the end, still set the currentCounter. see RecordPattern.
		*currentCounter = static_cast<CounterValue>(i - li);

		return {end, end};
	}

	/**
	* Records the size of successive runs of white and black pixels in a row, starting at a given point.
	* The values are recorded in the given array, and the number of runs recorded is equal to the size
	* of the array. If the row starts on a white pixel at the given start point, then the first count
	* recorded is the run of white pixels starting from that point; likewise it is the count of a run
	* of black pixels if the row begin on a black pixels at that point.
	*
	* @param row row to count from
	* @param start offset into row to start at
	* @param counters array into which to record counts
	* @throws NotFoundException if counters cannot be filled entirely from row before running out
	*  of pixels
	*/
	template <typename Iterator, typename Container>
	static Range<Iterator> RecordPattern(Iterator begin, Iterator end, Container& counters) {
		// mark the last counter-slot as empty
		counters.back() = 0;

		auto range = FindPattern(begin, end, counters, [](Iterator, Iterator, Container&) { return true; });

		// If we reached the end iterator but touched the last counter-slot, we accept the result.
		if (range.end == end && counters.back() != 0)
			return {begin, end};
		else
			return range;
	}

	template <typename Iterator, typename Container>
	static Range<Iterator> RecordPatternInReverse(Iterator begin, Iterator end, Container& counters) {
		std::reverse_iterator<Iterator> rbegin(end), rend(begin);
		auto range = RecordPattern(rbegin, rend, counters);
		std::reverse(counters.begin(), counters.end());
		return {range.end.base(), range.begin.base()};
	}

	/**
	* Determines how closely a set of observed counts of runs of black/white values matches a given
	* target pattern. This is reported as the ratio of the total variance from the expected pattern
	* proportions across all pattern elements, to the length of the pattern.
	*
	* @param counters observed counters
	* @param pattern expected pattern
	* @param maxIndividualVariance The most any counter can differ before we give up
	* @return ratio of total variance between counters and pattern compared to total pattern size
	*/
	template <typename Container>
	static float PatternMatchVariance(const Container& counters, const Container& pattern, float maxIndividualVariance) {
		assert(counters.size() <= pattern.size()); //TODO: this should test for equality, see ODCode128Reader.cpp:93
		return PatternMatchVariance(counters.data(), pattern.data(), counters.size(), maxIndividualVariance);
	}

	/**
	* Attempts to decode a sequence of black/white lines into single
	* digit.
	*
	* @param counters the counts of runs of observed black/white/black/... values
	* @param patterns the list of patterns to compare the contens of counters to
	* @param requireUnambiguousMatch the 'best match' must be better than all other matches
	* @return The decoded digit index, -1 if no pattern matched
	*/
	template <typename Patterns, typename Counters>
	static int DecodeDigit(const Counters& counters, const Patterns& patterns, float maxAvgVariance,
						   float maxIndividualVariance, bool requireUnambiguousMatch = true)
	{
		float bestVariance = maxAvgVariance; // worst variance we'll accept
		constexpr int INVALID_MATCH = -1;
		int bestMatch = INVALID_MATCH;
		for (size_t i = 0; i < patterns.size(); i++) {
			float variance = PatternMatchVariance(counters, patterns[i], maxIndividualVariance);
			if (variance < bestVariance) {
				bestVariance = variance;
				bestMatch = static_cast<int>(i);
			} else if (requireUnambiguousMatch && variance == bestVariance) {
				// if we find a second 'best match' with the same variance, we can not reliably report to have a suitable match
				bestMatch = INVALID_MATCH;
			}
		}
		return bestMatch;
	}

public:
	static float PatternMatchVariance(const int *counters, const int* pattern, size_t length, float maxIndividualVariance);

	/**
	 * @brief NarrowWideThreshold calculates width thresholds to separate narrow and wide bars and spaces.
	 *
	 * This is useful for codes like Codabar, Code39 and ITF which distinguish between narrow and wide
	 * bars/spaces. Where wide ones are between 2 and 3 times as wide as the narrow ones.
	 *
	 * @param view containing one character
	 * @return threshold value for bars and spaces
	 */
	static BarAndSpaceI NarrowWideThreshold(const PatternView& view)
	{
		BarAndSpaceI m = {std::numeric_limits<BarAndSpaceI::value_type>::max(),
						  std::numeric_limits<BarAndSpaceI::value_type>::max()};
		BarAndSpaceI M = {0, 0};
		for (int i = 0; i < view.size(); ++i) {
			m[i] = std::min(m[i], view[i]);
			M[i] = std::max(M[i], view[i]);
		}

		BarAndSpaceI res;
		for (int i = 0; i < 2; ++i) {
			// check that
			//  a) wide <= 4 * narrow
			//  b) bars and spaces are not more than a factor of 2 (or 3 for the max) apart from each other
			if (M[i] > 4 * (m[i] + 1) || M[i] > 3 * M[i + 1] || m[i] > 2 * (m[i + 1] + 1))
				return {};
			// the threshold is the average of min and max but at least 1.5 * min
			res[i] = std::max((m[i] + M[i]) / 2, m[i] * 3 / 2);
		}

		return res;
	}

	/**
	 * @brief ToNarrowWidePattern takes a PatternView, calculates a NarrowWideThreshold and returns int where a '0' bit
	 * means narrow and a '1' bit means 'wide'.
	 */
	static int ToNarrowWidePattern(const PatternView& view)
	{
		const auto threshold = NarrowWideThreshold(view);
		if (!threshold.isValid())
			return -1;

		int pattern = 0;
		for (int i = 0; i < view.size(); ++i) {
			if (view[i] > threshold[i] * 2)
				return -1;
			pattern = (pattern << 1) | (view[i] > threshold[i]);
		}

		return pattern;
	}

	template<typename INDEX, typename ALPHABET>
	static char DecodeNarrowWidePattern(const PatternView& view, const INDEX& table, const ALPHABET& alphabet)
	{
		int i = IndexOf(table, ToNarrowWidePattern(view));
		return i == -1 ? -1 : alphabet[i];
	}
};

} // OneD
} // ZXing
