#pragma once

#include <vector>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnString.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int TOO_MANY_ARGUMENTS_FOR_FUNCTION;
}

template <typename Name, typename Impl>
struct MultiSearchFirstPositionImpl
{
    using ResultType = UInt64;
    /// Variable for understanding, if we used offsets for the output, most
    /// likely to determine whether the function returns ColumnVector of ColumnArray.
    static constexpr bool is_column_array = false;
    static constexpr auto name = Name::name;

    static auto getReturnType() { return std::make_shared<DataTypeNumber<ResultType>>(); }

    static void vectorConstant(
        const ColumnString::Chars & haystack_data,
        const ColumnString::Offsets & haystack_offsets,
        const Array & needles_arr,
        PaddedPODArray<UInt64> & res,
        PaddedPODArray<UInt64> & /*offsets*/,
        bool /*allow_hyperscan*/,
        size_t /*max_hyperscan_regexp_length*/,
        size_t /*max_hyperscan_regexp_total_length*/,
        bool /*reject_expensive_hyperscan_regexps*/)
    {
        // For performance of Volnitsky search, it is crucial to save only one byte for pattern number.
        if (needles_arr.size() > std::numeric_limits<UInt8>::max())
            throw Exception(ErrorCodes::TOO_MANY_ARGUMENTS_FOR_FUNCTION,
                "Number of arguments for function {} doesn't match: passed {}, should be at most {}",
                name, std::to_string(needles_arr.size()), std::to_string(std::numeric_limits<UInt8>::max()));

        std::vector<std::string_view> needles;
        needles.reserve(needles_arr.size());
        for (const auto & needle : needles_arr)
            needles.emplace_back(needle.get<String>());

        auto res_callback = [](const UInt8 * start, const UInt8 * end) -> UInt64
        {
            return 1 + Impl::countChars(reinterpret_cast<const char *>(start), reinterpret_cast<const char *>(end));
        };
        auto searcher = Impl::createMultiSearcherInBigHaystack(needles);

        const size_t haystack_size = haystack_offsets.size();
        res.resize(haystack_size);

        size_t iteration = 0;
        while (searcher.hasMoreToSearch())
        {
            size_t prev_haystack_offset = 0;
            for (size_t j = 0; j < haystack_size; ++j)
            {
                const auto * haystack = &haystack_data[prev_haystack_offset];
                const auto * haystack_end = haystack + haystack_offsets[j] - prev_haystack_offset - 1;
                if (iteration == 0 || res[j] == 0)
                    res[j] = searcher.searchOneFirstPosition(haystack, haystack_end, res_callback);
                else
                {
                    UInt64 result = searcher.searchOneFirstPosition(haystack, haystack_end, res_callback);
                    if (result != 0)
                        res[j] = std::min(result, res[j]);
                }
                prev_haystack_offset = haystack_offsets[j];
            }
            ++iteration;
        }
        if (iteration == 0)
            std::fill(res.begin(), res.end(), 0);
    }

    static void vectorVector(
        const ColumnString::Chars & haystack_data,
        const ColumnString::Offsets & haystack_offsets,
        const IColumn & needles_data,
        const ColumnArray::Offsets & needles_offsets,
        PaddedPODArray<ResultType> & res,
        PaddedPODArray<UInt64> & /*offsets*/,
        bool /*allow_hyperscan*/,
        size_t /*max_hyperscan_regexp_length*/,
        size_t /*max_hyperscan_regexp_total_length*/,
        bool /*reject_expensive_hyperscan_regexps*/)
    {
        const size_t haystack_size = haystack_offsets.size();
        res.resize(haystack_size);

        size_t prev_haystack_offset = 0;
        size_t prev_needles_offset = 0;

        const ColumnString * needles_data_string = checkAndGetColumn<ColumnString>(&needles_data);

        std::vector<std::string_view> needles;

        auto res_callback = [](const UInt8 * start, const UInt8 * end) -> UInt64
        {
            return 1 + Impl::countChars(reinterpret_cast<const char *>(start), reinterpret_cast<const char *>(end));
        };

        for (size_t i = 0; i < haystack_size; ++i)
        {
            needles.reserve(needles_offsets[i] - prev_needles_offset);

            for (size_t j = prev_needles_offset; j < needles_offsets[i]; ++j)
            {
                needles.emplace_back(needles_data_string->getDataAt(j).toView());
            }

            auto searcher = Impl::createMultiSearcherInBigHaystack(needles); // sub-optimal

            const auto * const haystack = &haystack_data[prev_haystack_offset];
            const auto * haystack_end = haystack + haystack_offsets[i] - prev_haystack_offset - 1;

            size_t iteration = 0;
            while (searcher.hasMoreToSearch())
            {
                if (iteration == 0 || res[i] == 0)
                {
                    res[i] = searcher.searchOneFirstPosition(haystack, haystack_end, res_callback);
                }
                else
                {
                    UInt64 result = searcher.searchOneFirstPosition(haystack, haystack_end, res_callback);
                    if (result != 0)
                    {
                        res[i] = std::min(result, res[i]);
                    }
                }
                ++iteration;
            }
            if (iteration == 0)
            {
                res[i] = 0;
            }

            prev_haystack_offset = haystack_offsets[i];
            prev_needles_offset = needles_offsets[i];
            needles.clear();
        }
    }
};

}
