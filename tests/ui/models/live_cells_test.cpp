#include <catch2/catch.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

#include "ui/live_values.hpp"
#include "ui/models/live_cells.hpp"

namespace
{
    std::vector<std::byte> bytes(std::initializer_list<unsigned> values)
    {
        std::vector<std::byte> out;
        out.reserve(values.size());
        for (const unsigned value : values)
        {
            out.push_back(static_cast<std::byte>(value));
        }
        return out;
    }

    slopkit::ui::LiveReading readable(std::size_t id, std::vector<std::byte> value)
    {
        return slopkit::ui::LiveReading {.id = id, .readable = true, .bytes = std::move(value)};
    }

    slopkit::ui::models::LiveCandidate candidate(std::size_t row, std::uint64_t identity)
    {
        return {.address = 0x1000 + row, .size = 4, .identity = identity, .readable = true};
    }

    using Run = std::pair<std::size_t, std::size_t>;
}

TEST_CASE("live cells request only the readable rows", "[ui][models]")
{
    slopkit::ui::models::LiveCells              cells;
    const std::vector<slopkit::ui::LiveRequest> requests =
        cells.request(3,
                      [](std::size_t row)
                      {
                          if (row == 1)
                          {
                              return slopkit::ui::models::LiveCandidate {.readable = false};
                          }
                          return candidate(row, 100 + row);
                      });

    REQUIRE(requests.size() == 2);
    CHECK(requests[0].id == 0);
    CHECK(requests[0].address == 0x1000);
    CHECK(requests[0].size == 4);
    CHECK(requests[1].id == 2);
    CHECK(cells.size() == 3);
    CHECK(cells.at(0) != nullptr);
    CHECK(cells.at(3) == nullptr);
}

TEST_CASE("live cells coalesce changed rows into runs", "[ui][models]")
{
    slopkit::ui::models::LiveCells cells;
    (void)cells.request(4,
                        [](std::size_t row)
                        {
                            return candidate(row, 100 + row);
                        });

    std::vector<Run> runs;
    const auto       emit_run = [&runs](std::size_t first, std::size_t last)
    {
        runs.emplace_back(first, last);
    };
    const auto valid = [](std::size_t, std::uint64_t)
    {
        return true;
    };

    cells.apply(
        std::vector<slopkit::ui::LiveReading> {
            readable(0, bytes({1})), readable(1, bytes({2})), readable(2, bytes({3})), readable(3, bytes({4}))},
        valid,
        emit_run);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0] == Run {0, 3});

    runs.clear();
    (void)cells.request(4,
                        [](std::size_t row)
                        {
                            return candidate(row, 100 + row);
                        });
    cells.apply(
        std::vector<slopkit::ui::LiveReading> {
            readable(0, bytes({1})), readable(1, bytes({2})), readable(2, bytes({3})), readable(3, bytes({4}))},
        valid,
        emit_run);
    CHECK(runs.empty());

    runs.clear();
    (void)cells.request(4,
                        [](std::size_t row)
                        {
                            return candidate(row, 100 + row);
                        });
    cells.apply(
        std::vector<slopkit::ui::LiveReading> {
            readable(0, bytes({9})), readable(1, bytes({2})), readable(2, bytes({7})), readable(3, bytes({4}))},
        valid,
        emit_run);
    REQUIRE(runs.size() == 2);
    CHECK(runs[0] == Run {0, 0});
    CHECK(runs[1] == Run {2, 2});
}

TEST_CASE("live cells drop a reading whose row moved on", "[ui][models]")
{
    slopkit::ui::models::LiveCells cells;
    std::vector<std::uint64_t>     identities {100, 101};
    const auto                     valid = [&identities](std::size_t row, std::uint64_t identity)
    {
        return row < identities.size() && identities[row] == identity;
    };
    std::size_t runs = 0;

    (void)cells.request(2,
                        [](std::size_t row)
                        {
                            return candidate(row, 100 + row);
                        });
    cells.apply(std::vector<slopkit::ui::LiveReading> {readable(0, bytes({5})), readable(1, bytes({6}))},
                valid,
                [&runs](std::size_t, std::size_t)
                {
                    ++runs;
                });
    REQUIRE(runs == 1);
    const std::optional<std::vector<std::byte>> first = cells.at(0)->bytes;
    REQUIRE(first.has_value());
    CHECK(*first == bytes({5}));
    CHECK_FALSE(cells.at(0)->changed);

    identities[0] = 999;
    runs          = 0;
    (void)cells.request(2,
                        [](std::size_t row)
                        {
                            return candidate(row, 100 + row);
                        });
    cells.apply(std::vector<slopkit::ui::LiveReading> {readable(0, bytes({7})), readable(1, bytes({6}))},
                valid,
                [&runs](std::size_t, std::size_t)
                {
                    ++runs;
                });
    CHECK(runs == 0);
    CHECK(cells.at(0)->bytes == first);
    CHECK_FALSE(cells.at(0)->changed);
    CHECK(cells.at(1)->has_reading);
}

TEST_CASE("live cells leave the tail of a short batch alone", "[ui][models]")
{
    slopkit::ui::models::LiveCells cells;
    const auto                     valid = [](std::size_t, std::uint64_t)
    {
        return true;
    };

    (void)cells.request(3,
                        [](std::size_t row)
                        {
                            return candidate(row, 100 + row);
                        });
    cells.apply(std::vector<slopkit::ui::LiveReading> {readable(0, bytes({1})), readable(1, bytes({2}))},
                valid,
                [](std::size_t, std::size_t) {});
    CHECK(cells.at(0)->has_reading);
    CHECK(cells.at(1)->has_reading);
    CHECK_FALSE(cells.at(2)->has_reading);

    (void)cells.request(3,
                        [](std::size_t row)
                        {
                            return candidate(row, 100 + row);
                        });
    cells.apply(std::vector<slopkit::ui::LiveReading> {readable(2, bytes({1})),
                                                       readable(1, bytes({2})),
                                                       readable(0, bytes({3}))},
                valid,
                [](std::size_t, std::size_t) {});
    REQUIRE(cells.at(0)->bytes.has_value());
    CHECK(*cells.at(0)->bytes == bytes({1}));
    CHECK(cells.at(1)->has_reading);
    CHECK_FALSE(cells.at(2)->has_reading);
}

TEST_CASE("live cells record an unreadable address without a change", "[ui][models]")
{
    slopkit::ui::models::LiveCells cells;
    const auto                     valid = [](std::size_t, std::uint64_t)
    {
        return true;
    };

    (void)cells.request(1,
                        [](std::size_t row)
                        {
                            return candidate(row, 100 + row);
                        });
    cells.apply(
        std::vector<slopkit::ui::LiveReading> {
            slopkit::ui::LiveReading {.id = 0, .readable = false, .bytes = {}}
    },
        valid,
        [](std::size_t, std::size_t) {});

    REQUIRE(cells.at(0) != nullptr);
    CHECK(cells.at(0)->has_reading);
    CHECK_FALSE(cells.at(0)->readable);
    CHECK_FALSE(cells.at(0)->changed);
    CHECK_FALSE(cells.at(0)->bytes.has_value());
}

TEST_CASE("live cells flag only the bytes that moved", "[ui][models]")
{
    slopkit::ui::models::LiveCells cells;
    const auto                     valid = [](std::size_t, std::uint64_t)
    {
        return true;
    };
    const auto pass = [&cells, &valid](unsigned first, unsigned second, unsigned third, unsigned fourth)
    {
        (void)cells.request(1,
                            [](std::size_t row)
                            {
                                return candidate(row, 100 + row);
                            });
        cells.apply(std::vector<slopkit::ui::LiveReading> {readable(0, bytes({first, second, third, fourth}))},
                    valid,
                    [](std::size_t, std::size_t) {});
    };

    pass(0x1A, 0x2B, 0x3C, 0x4D);
    CHECK(cells.at(0)->changed_bytes == std::vector<std::uint8_t> {0, 0, 0, 0});
    CHECK_FALSE(cells.at(0)->changed);

    pass(0x1A, 0x00, 0x3C, 0x4D);
    CHECK(cells.at(0)->changed_bytes == std::vector<std::uint8_t> {0, 1, 0, 0});
    CHECK(cells.at(0)->changed);

    pass(0x1A, 0x00, 0x3C, 0x4D);
    CHECK(cells.at(0)->changed_bytes == std::vector<std::uint8_t> {0, 0, 0, 0});
    CHECK_FALSE(cells.at(0)->changed);
}

TEST_CASE("live cells drop the byte mask when a block becomes unreadable", "[ui][models]")
{
    slopkit::ui::models::LiveCells cells;
    const auto                     valid = [](std::size_t, std::uint64_t)
    {
        return true;
    };

    (void)cells.request(1,
                        [](std::size_t row)
                        {
                            return candidate(row, 100 + row);
                        });
    cells.apply(std::vector<slopkit::ui::LiveReading> {readable(0, bytes({0x1A, 0x2B, 0x3C, 0x4D}))},
                valid,
                [](std::size_t, std::size_t) {});

    (void)cells.request(1,
                        [](std::size_t row)
                        {
                            return candidate(row, 100 + row);
                        });
    cells.apply(std::vector<slopkit::ui::LiveReading> {readable(0, bytes({0x1A, 0x00, 0x3C, 0x4D}))},
                valid,
                [](std::size_t, std::size_t) {});
    REQUIRE(cells.at(0)->changed);

    (void)cells.request(1,
                        [](std::size_t row)
                        {
                            return candidate(row, 100 + row);
                        });
    cells.apply(
        std::vector<slopkit::ui::LiveReading> {
            slopkit::ui::LiveReading {.id = 0, .readable = false, .bytes = {}}
    },
        valid,
        [](std::size_t, std::size_t) {});
    CHECK(cells.at(0)->changed_bytes.empty());
    CHECK_FALSE(cells.at(0)->changed);
}

TEST_CASE("live cells forget the byte mask when the row set is reset", "[ui][models]")
{
    slopkit::ui::models::LiveCells cells;
    const auto                     valid = [](std::size_t, std::uint64_t)
    {
        return true;
    };

    (void)cells.request(2,
                        [](std::size_t row)
                        {
                            return candidate(row, 100 + row);
                        });
    cells.apply(
        std::vector<slopkit::ui::LiveReading> {readable(0, bytes({1, 2, 3, 4})), readable(1, bytes({5, 6, 7, 8}))},
        valid,
        [](std::size_t, std::size_t) {});

    (void)cells.request(2,
                        [](std::size_t row)
                        {
                            return candidate(row, 100 + row);
                        });
    cells.apply(
        std::vector<slopkit::ui::LiveReading> {readable(0, bytes({1, 9, 3, 4})), readable(1, bytes({5, 6, 7, 8}))},
        valid,
        [](std::size_t, std::size_t) {});
    REQUIRE(cells.at(0)->changed);

    cells.reset(2);
    CHECK(cells.at(0)->changed_bytes.empty());
    CHECK_FALSE(cells.at(0)->changed);
}

TEST_CASE("live cells keep a reading across an invalidate", "[ui][models]")
{
    slopkit::ui::models::LiveCells cells;
    const auto                     valid = [](std::size_t, std::uint64_t)
    {
        return true;
    };
    std::size_t runs     = 0;
    const auto  emit_run = [&runs](std::size_t, std::size_t)
    {
        ++runs;
    };

    (void)cells.request(1,
                        [](std::size_t row)
                        {
                            return candidate(row, 100 + row);
                        });
    cells.apply(std::vector<slopkit::ui::LiveReading> {readable(0, bytes({1, 2, 3, 4}))}, valid, emit_run);
    REQUIRE(cells.at(0)->has_reading);
    REQUIRE(cells.at(0)->bytes.has_value());

    // An invalidate drops only the request that was in flight; the reading stays.
    cells.invalidate();
    CHECK(cells.at(0)->has_reading);
    CHECK(cells.at(0)->readable);
    REQUIRE(cells.at(0)->bytes.has_value());
    CHECK(*cells.at(0)->bytes == bytes({1, 2, 3, 4}));

    // With no request in flight a following batch applies nothing.
    runs = 0;
    cells.apply(std::vector<slopkit::ui::LiveReading> {readable(0, bytes({9, 9, 9, 9}))}, valid, emit_run);
    CHECK(runs == 0);
    CHECK(*cells.at(0)->bytes == bytes({1, 2, 3, 4}));

    // A reading for another identity describes different bytes: it is the cell's
    // first and flags nothing.
    (void)cells.request(1,
                        [](std::size_t row)
                        {
                            return candidate(row, 200 + row);
                        });
    cells.apply(std::vector<slopkit::ui::LiveReading> {readable(0, bytes({9, 9, 9, 9}))}, valid, emit_run);
    CHECK_FALSE(cells.at(0)->changed);
    CHECK(cells.at(0)->changed_bytes == std::vector<std::uint8_t> {0, 0, 0, 0});

    // A reading of another size flags nothing either.
    (void)cells.request(1,
                        [](std::size_t row)
                        {
                            return candidate(row, 200 + row);
                        });
    cells.apply(std::vector<slopkit::ui::LiveReading> {readable(0, bytes({9, 9}))}, valid, emit_run);
    CHECK_FALSE(cells.at(0)->changed);
    CHECK(cells.at(0)->changed_bytes == std::vector<std::uint8_t> {0, 0});
}
