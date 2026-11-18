// PatternPlanner.h -- decides what a genre template or a random roll actually means for
// this kit, without touching the engine, the UI or JUCE.
//
// Kept free of JUCE on purpose: the whole decision is pure data, so Tests\PatternPlanTest.cpp
// compiles it with cl alone and the rules can be argued about by running them. The plugin side
// supplies LaneInfo (does this lane hold a sample, what is it called) and applies the resulting
// grid through `GrooveboxProcessor::generateGenreTake`, which hands the twelve rows to
// `writeSlotRows` as one bulk write into one slot -- not through the per-cell `setStep` and
// `setStepVelocity` seams, because those resolve the target slot from the lane's own pointer and
// would therefore overwrite whatever the user is currently hearing.
//
// The rule the user picked is "roles onto lanes that have a sample". Concretely, for one core of
// one genre:
//
//   1. sort the template rows into a drum hierarchy -- kick, then snare, then clap, then hats --
//      and inside a family by the pad index the source table gives them, so the main kick is
//      placed before its layer and the closed hat before the open one;
//   2. for each row, take the LOWEST-indexed lane that has a sample, classifies to that family,
//      and is not already used;
//   3. if nothing in the kit is named for that family, fall back to the lowest-indexed unused
//      lane whose name classifies to nothing at all -- an unnamed sample is free real estate, a
//      named one is the user's decision and is never stolen from its family;
//   4. a row that gets neither is reported as dropped, and the caller says so instead of
//      silently writing a grid the user cannot hear.
//
// Rows never share a lane, so no two template parts have to be merged, and every lane the plan
// does not place is left at silence so a generated grid replaces the pattern outright rather
// than overdubbing the old one.

#pragma once

#include <cstddef>
#include <cstring>

#include "GenrePatterns.h"

namespace PatternPlanner
{
    /** A genre template has to be able to fill the whole kit. */
    inline constexpr int maxLanes = 12;

    struct LaneInfo
    {
        bool hasSample = false;
        const char* sampleName = "";    // file name the sample came from
        const char* channelName = "";   // what the strip is called
    };

    struct Placement
    {
        const GenrePatterns::Row* row = nullptr;
        int lane = -1;
        bool byName = false;    // matched the family by name, versus an unnamed lane
    };

    struct Plan
    {
        GenrePatterns::Family family = GenrePatterns::Family::kick;

        /** 16 velocities per lane; 0.0f where nothing fires. Complete, so the caller can
            write the grid in one pass without reading what was there before. */
        float velocity[maxLanes][GenrePatterns::numSteps] {};

        Placement placements[maxLanes] {};
        int numPlaced = 0;

        /** Template rows that found no lane, counted per family -- what a report line reads. */
        int dropped[4] {};

        int totalRows = 0;      // rows the core asked for, for the same report
    };

    //==========================================================================
    // Naming
    //==========================================================================

    /**
        Which drum family a name belongs to, or nullptr when it says nothing.

        The keywords come from the two products already in the house: drumgen classifies a
        sample by its label (KICK/808/TOM -> kick, SNARE/GHOST -> snare, HAT/O-HAT/RIDE -> hat,
        everything else -> clap) and its pad table calls the four families kick, snare, clap and
        hat; Groovebox names its twelve strips Kick, Snare, Hat, Clap, Tom, Rim, Cowbell, Conga,
        Shaker, Crash, Ride, Perc. Substring, case-insensitive, first family to match wins in the
        order kick, snare, clap, hat -- so "808kick" and "kick_layer2" are kicks and "open_hat"
        is a hat.

        Tom and its relatives go to kick, the way drumgen does it: a tom is a tuned membrane,
        and a genre's kick lane is where a low membrane belongs.
    */
    inline const GenrePatterns::Family* familyOfName (const char* name) noexcept
    {
        if (name == nullptr || *name == '\0')
            return nullptr;

        char lower[96];
        std::size_t i = 0;
        for (; i + 1 < sizeof (lower) && name[i] != '\0'; ++i)
            lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (char) (name[i] + 32) : name[i];
        lower[i] = '\0';

        static constexpr GenrePatterns::Family kick  = GenrePatterns::Family::kick;
        static constexpr GenrePatterns::Family snare = GenrePatterns::Family::snare;
        static constexpr GenrePatterns::Family clap  = GenrePatterns::Family::clap;
        static constexpr GenrePatterns::Family hat   = GenrePatterns::Family::hat;

        static const char* const kickWords[] = { "kick", "808", "sub", "bassdrum", "bass drum",
                                                 "bd", "punch", "tom", "conga", "bongo" };
        static const char* const snareWords[] = { "snare", "sd", "crack", "ghost" };
        static const char* const clapWords[]  = { "clap", "cp", "handclap", "rim", "snap" };
        static const char* const hatWords[]   = { "hat", "hh", "chh", "cymbal", "ride", "crash",
                                                  "shaker", "bell", "metal", "air", "open",
                                                  "closed", "tick", "perc" };

        const auto has = [&lower] (const char* const* words, std::size_t count) {
            for (std::size_t w = 0; w < count; ++w)
                if (std::strstr (lower, words[w]) != nullptr)
                    return true;
            return false;
        };

        if (has (kickWords, sizeof (kickWords) / sizeof (kickWords[0])))  return &kick;
        if (has (snareWords, sizeof (snareWords) / sizeof (snareWords[0]))) return &snare;
        if (has (clapWords, sizeof (clapWords) / sizeof (clapWords[0])))  return &clap;
        if (has (hatWords, sizeof (hatWords) / sizeof (hatWords[0])))     return &hat;
        return nullptr;
    }

    /** Reads a lane's own name first, then its strip name: a file called Rim.wav on a strip
        called Hat is a rim, because that is the thing the user loaded. */
    inline const GenrePatterns::Family* familyOfLane (const LaneInfo& lane) noexcept
    {
        if (const auto* byFile = familyOfName (lane.sampleName))
            return byFile;
        return familyOfName (lane.channelName);
    }

    //==========================================================================
    // Ordering
    //==========================================================================

    /** The drum hierarchy rows are placed in. */
    inline constexpr int familyRank (const GenrePatterns::Family f) noexcept
    {
        switch (f)
        {
            case GenrePatterns::Family::kick: return 0;
            case GenrePatterns::Family::snare: return 1;
            case GenrePatterns::Family::clap: return 2;
            case GenrePatterns::Family::hat: break;
        }
        return 3;
    }

    /**
        Pad index of a role in the source table -- the order a 909 kit lays its pads out in,
        which is also a sensible emphasis order (k before k2, s before s2, the main clap before
        its layers, the closed hat before the open one).
    */
    inline int padIndex (const char* role) noexcept
    {
        static const char* const order[] = { "k", "k2", "s", "c", "s2", "a", "h", "m",
                                             "m2", "m3", "c2", "c3", "o", "a2", "c4", "c5" };
        for (std::size_t i = 0; i < sizeof (order) / sizeof (order[0]); ++i)
            if (std::strcmp (order[i], role) == 0)
                return (int) i;
        return 16;    // unknown role sorts last rather than being dropped silently
    }

    //==========================================================================
    // Genre plan
    //==========================================================================

    /**
        Which variant of a genre a seed asks for. Seed 0 is the first core the table carries, so
        the default is the documented pattern and never a random one; rolling advances the seed.
    */
    inline int coreFor (const GenrePatterns::Genre& genre, const unsigned int seed) noexcept
    {
        if (genre.numCores <= 0)
            return 0;
        return (int) (seed % (unsigned int) genre.numCores);
    }

    /** Builds the complete 12 x 16 velocity grid one genre core implies for this kit. */
    inline Plan makePlan (const GenrePatterns::Genre& genre,
                          const int coreIndex,
                          const LaneInfo* lanes,
                          const int numLanes) noexcept
    {
        Plan plan {};

        if (numLanes <= 0 || numLanes > maxLanes)
            return plan;

        const int ci = coreIndex >= 0 && coreIndex < genre.numCores ? coreIndex : 0;
        const GenrePatterns::Core& core = genre.cores[ci];

        // Ordered view of the rows, without touching the generated data.
        const GenrePatterns::Row* ordered[GenrePatterns::numSteps * 2] {};
        int numOrdered = 0;
        for (int r = 0; r < core.numRows && numOrdered < (int) (sizeof (ordered) / sizeof (ordered[0])); ++r)
        {
            const auto* row = &core.rows[r];
            int at = numOrdered++;
            while (at > 0)
            {
                const auto* prev = ordered[at - 1];
                const bool later = familyRank (row->family) < familyRank (prev->family)
                                   || (familyRank (row->family) == familyRank (prev->family)
                                       && padIndex (row->role) < padIndex (prev->role));
                if (! later)
                    break;
                ordered[at] = prev;
                --at;
            }
            ordered[at] = row;
        }

        bool used[maxLanes] {};
        plan.family = core.numRows > 0 ? core.rows[0].family : GenrePatterns::Family::kick;
        plan.totalRows = core.numRows;

        for (int r = 0; r < numOrdered; ++r)
        {
            const auto* row = ordered[r];
            int lane = -1;
            bool byName = false;

            for (int i = 0; i < numLanes && lane < 0; ++i)
            {
                if (used[i] || ! lanes[i].hasSample)
                    continue;
                const auto* family = familyOfLane (lanes[i]);
                if (family != nullptr && *family == row->family)
                {
                    lane = i;
                    byName = true;
                }
            }

            // Nothing in the kit carries this family's name, so an unnamed lane may take it --
            // lowest index first, so the same kit and the same template always land the same way.
            for (int i = 0; i < numLanes && lane < 0; ++i)
            {
                if (used[i] || ! lanes[i].hasSample)
                    continue;
                if (familyOfLane (lanes[i]) == nullptr)
                    lane = i;
            }

            if (lane < 0)
            {
                ++plan.dropped[familyRank (row->family)];
                continue;
            }

            used[lane] = true;
            plan.placements[plan.numPlaced].row = row;
            plan.placements[plan.numPlaced].lane = lane;
            plan.placements[plan.numPlaced].byName = byName;
            ++plan.numPlaced;

            for (int step = 0; step < GenrePatterns::numSteps; ++step)
                plan.velocity[lane][step] = GenrePatterns::velocityFor (GenrePatterns::accentFor (row->pattern[step]));
        }

        return plan;
    }

    //==========================================================================
    // Random sample roll
    //==========================================================================

    /**
        Next value of the roll counter. xorshift32 rather than std::mt19937 plus a distribution,
        because a distribution's mapping is implementation-defined and the whole point is that a
        roll can be replayed, reported and tested with one number.
    */
    inline unsigned int nextSeed (unsigned int seed) noexcept
    {
        if (seed == 0u)
            seed = 0x9E3779B9u;
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed;
    }

    /**
        Index of a pool entry to load, or -1 when there is nothing to pick.

        The current entry is excluded so a roll is always a change, unless the pool holds one
        file. Deterministic for (pool, index, seed), which is what makes it testable and what
        keeps a saved session able to explain its own last pick.
    */
    inline int pickFromPool (const int poolSize,
                             const int currentIndex,
                             const unsigned int seed) noexcept
    {
        if (poolSize <= 0)
            return -1;
        if (poolSize == 1)
            return 0;

        const bool excludeCurrent = currentIndex >= 0 && currentIndex < poolSize;
        const int choices = poolSize - (excludeCurrent ? 1 : 0);
        int index = (int) (nextSeed (seed) % (unsigned int) choices);
        if (excludeCurrent && index >= currentIndex)
            ++index;
        return index;
    }
}
