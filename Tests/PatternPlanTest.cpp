// PatternPlanTest.cpp -- the genre table and the planner, checked as plain data.
//
//   cl /std:c++17 /EHsc /nologo /W4 /I..\Source PatternPlanTest.cpp /Fe:..\out\PatternPlanTest.exe
//
// Deliberately NOT wired into CMake and NOT linking JUCE. The two headers it exercises are
// std-only by design (GenrePatterns.h is generated data, PatternPlanner.h is the placement
// rule), so they can be compiled and run by themselves in a second, and the smoke suite --
// which currently belongs to another session's in-flight work -- stays untouched.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../Source/GenrePatterns.h"
#include "../Source/PatternPlanner.h"

namespace
{
    int checks  = 0;
    int problems = 0;

    void report (const bool ok, const char* what)
    {
        ++checks;
        if (ok)
            return;
        ++problems;
        std::printf ("  PROBLEM  %s\n", what);
    }

    void reportInt (const long long got, const long long want, const char* what)
    {
        ++checks;
        if (got == want)
            return;
        ++problems;
        std::printf ("  PROBLEM  %s -- got %lld, want %lld\n", what, got, want);
    }

    void reportFloat (const float got, const float want, const char* what)
    {
        ++checks;
        if (got == want)
            return;
        ++problems;
        std::printf ("  PROBLEM  %s -- got %.4f, want %.4f\n", what, (double) got, (double) want);
    }

    using namespace PatternPlanner;

    /** The twelve strip names the plugin ships with. */
    static const char* const defaultStrips[] = { "Kick", "Snare", "Hat", "Clap", "Tom", "Rim",
                                                 "Cowbell", "Conga", "Shaker", "Crash", "Ride", "Perc" };

    /** A twelve-lane kit, all of it loaded, names straight from the strips. */
    void fillKit (const char* const* fileNames, LaneInfo (&lanes)[maxLanes])
    {
        for (int i = 0; i < maxLanes; ++i)
        {
            lanes[i].hasSample   = fileNames != nullptr && fileNames[i] != nullptr;
            lanes[i].sampleName  = lanes[i].hasSample ? fileNames[i] : "";
            lanes[i].channelName = defaultStrips[i];
        }
    }

    /** The same, with the strip names the user typed -- every default strip name classifies
        to a family, so the unnamed-lane fallback can only be reached through a rename. */
    void fillNamedKit (const char* const* fileNames, const char* const* strips,
                       LaneInfo (&lanes)[maxLanes])
    {
        for (int i = 0; i < maxLanes; ++i)
        {
            lanes[i].hasSample   = fileNames != nullptr && fileNames[i] != nullptr;
            lanes[i].sampleName  = lanes[i].hasSample ? fileNames[i] : "";
            lanes[i].channelName = strips[i];
        }
    }

    // FNV-1a 64, matching the emitter's canonical text so a ported row cannot drift
    // unnoticed. GenrePatterns.h's banner carries the expected value.
    std::uint64_t fnv1a64OfCanonical()
    {
        std::uint64_t h = 0xcbf29ce484222325ull;
        constexpr std::uint64_t prime = 0x100000001b3ull;
        auto feed = [&h, &prime] (const char* s) {
            for (; *s != '\0'; ++s)
            {
                h ^= (std::uint64_t) (unsigned char) *s;
                h *= prime;
            }
        };
        auto feedChar = [&h, &prime] (char c) {
            h ^= (std::uint64_t) (unsigned char) c;
            h *= prime;
        };

        bool firstLine = true;
        for (int g = 0; g < GenrePatterns::numGenres; ++g)
        {
            const auto& genre = GenrePatterns::genres[g];
            for (int ci = 0; ci < genre.numCores; ++ci)
            {
                const auto& core = genre.cores[ci];
                for (int r = 0; r < core.numRows; ++r)
                {
                    const auto& row = core.rows[r];
                    if (! firstLine)
                        feedChar ('\n');
                    firstLine = false;

                    feed (genre.id);
                    feedChar ('|');
                    char prefix[4];
                    std::snprintf (prefix, sizeof (prefix), "%d", ci);
                    feed (prefix);
                    feedChar ('|');
                    feed (row.role);
                    feedChar ('|');
                    feed (GenrePatterns::familyNames[(int) row.family]);
                    feedChar ('|');
                    feed (row.pattern);
                }
            }
        }
        return h;
    }
}

int main()
{
    std::printf ("PatternPlanTest -- genre table fidelity and planner behaviour\n");

    //======================================================================
    std::printf ("\n[1] the ported table is still drumgen's table\n");
    {
        int genres = GenrePatterns::numGenres;
        reportInt (genres, 29, "29 genres ported");
        reportInt ((int) (sizeof (GenrePatterns::genreIds) / sizeof (GenrePatterns::genreIds[0])),
                   genres, "genreIds covers every genre");

        int cores = 0, rows = 0, badLen = 0, badChar = 0, badFamily = 0;
        for (int g = 0; g < genres; ++g)
        {
            const auto& genre = GenrePatterns::genres[g];
            if (std::strcmp (genre.id, GenrePatterns::genreIds[g]) != 0)
                ++badFamily;
            cores += genre.numCores;
            for (int ci = 0; ci < genre.numCores; ++ci)
            {
                const auto& core = genre.cores[ci];
                for (int r = 0; r < core.numRows; ++r)
                {
                    const auto& row = core.rows[r];
                    ++rows;
                    if (std::strlen (row.pattern) != (size_t) GenrePatterns::numSteps)
                        ++badLen;
                    for (int i = 0; i < GenrePatterns::numSteps; ++i)
                    {
                        const char c = row.pattern[i];
                        if (c != '.' && c != 'X' && c != 'x' && c != 'o')
                            ++badChar;
                    }
                    const int f = (int) row.family;
                    if (f < 0 || f > 3)
                        ++badFamily;
                }
            }
        }
        reportInt (cores, 116, "116 cores");
        reportInt (rows, 533, "533 rows");
        reportInt (badLen, 0, "every row is exactly 16 steps");
        reportInt (badChar, 0, "only the four source characters appear");
        reportInt (badFamily, 0, "ids and families are in range");

        // Independent of the port script: recompute the canonical digest from the header's
        // own data and compare it to the number the generator printed from the JS object.
        // This is the gate that says nobody hand-tuned a pattern afterwards.
        report (fnv1a64OfCanonical() == 0x9d7b42919b918620ull,
                "canonical table digest matches the ported source");

        // Every genre must be able to fill a bar somewhere; an empty core would generate
        // silence and read as a bug in the kit instead of the table.
        int emptyCores = 0, emptyGenres = 0;
        for (int g = 0; g < genres; ++g)
        {
            const auto& genre = GenrePatterns::genres[g];
            bool anyRow = false;
            for (int ci = 0; ci < genre.numCores; ++ci)
            {
                if (genre.cores[ci].numRows <= 0)
                    ++emptyCores;
                else
                    anyRow = true;
            }
            if (! anyRow)
                ++emptyGenres;
        }
        reportInt (emptyCores, 0, "no core is rowless");
        reportInt (emptyGenres, 0, "no genre is entirely empty");
    }

    //======================================================================
    std::printf ("\n[2] the three accents mean the three velocities the source uses\n");
    {
        namespace G = GenrePatterns;
        report (G::accentFor ('X') == G::Accent::full,   "'X' is a full hit");
        report (G::accentFor ('x') == G::Accent::push,   "'x' is a push");
        report (G::accentFor ('o') == G::Accent::ghost,  "'o' is a ghost");
        report (G::accentFor ('.') == G::Accent::silent, "'.' is silent");
        report (G::accentFor ('q') == G::Accent::silent, "anything else is silent");

        reportFloat (G::velocityFor (G::Accent::full),   1.00f, "full velocity is GENRE_VEL X");
        reportFloat (G::velocityFor (G::Accent::push),   0.72f, "push velocity is GENRE_VEL x");
        reportFloat (G::velocityFor (G::Accent::ghost),  0.48f, "ghost velocity is GENRE_VEL o");
        reportFloat (G::velocityFor (G::Accent::silent), 0.00f, "silent is silence");

        // The grid writer uses the level to decide on/off, so a ghost must not round to zero.
        report (G::velocityFor (G::Accent::ghost) > 0.0f, "a ghost is audible, not off");
    }

    //======================================================================
    std::printf ("\n[3] roles land on lanes that have a sample\n");
    {
        // A kit where every strip has its own sound: the template's kick goes to the kick
        // lane, its snare to the snare lane, and no two rows share a lane.
        static const char* const fullKit[] = {
            "808kick.wav", "snare.wav", "closed_hat.wav", "clap.wav", "tom.wav", "rim.wav",
            "cowbell.wav", "conga.wav", "shaker.wav", "crash.wav", "ride.wav", "perc.wav" };
        LaneInfo lanes[maxLanes];
        fillKit (fullKit, lanes);
        const auto& genre = GenrePatterns::genres[0];    // techno
        const auto plan = makePlan (genre, 0, &lanes[0], maxLanes);

        report (plan.numPlaced > 0, "a full kit gets a pattern");
        report (plan.totalRows == genre.cores[0].numRows, "the plan reports how many rows the core asked for");

        int reused = 0, misfiled = 0, outOfRange = 0, silentRows = 0;
        bool seen[maxLanes] {};
        for (int p = 0; p < plan.numPlaced; ++p)
        {
            const auto& pl = plan.placements[p];
            if (pl.lane < 0 || pl.lane >= maxLanes)
                ++outOfRange;
            else if (seen[pl.lane])
                ++reused;
            else
                seen[pl.lane] = true;

            if (pl.row != nullptr && pl.byName)
            {
                const auto* f = familyOfLane (lanes[pl.lane]);
                if (f == nullptr || *f != pl.row->family)
                    ++misfiled;
            }

            bool any = false;
            for (int s = 0; s < GenrePatterns::numSteps; ++s)
                if (plan.velocity[pl.lane][s] > 0.0f)
                    any = true;
            if (! any)
                ++silentRows;
        }
        reportInt (reused, 0, "no two template rows share a lane");
        reportInt (misfiled, 0, "every placement is on a lane of the same family");
        reportInt (outOfRange, 0, "placements address real lanes");
        reportInt (silentRows, 0, "every placed row fires somewhere");
        reportInt (plan.numPlaced + plan.dropped[0] + plan.dropped[1]
                   + plan.dropped[2] + plan.dropped[3], plan.totalRows,
                   "placed plus dropped equals the rows asked for");

        // The kick row of techno core 0 is X...X...X...X...; the plan must reproduce that
        // exact law on the lane it chose, not merely something kick-shaped.
        int kickRow = -1;
        for (int p = 0; p < plan.numPlaced; ++p)
            if (plan.placements[p].row != nullptr && std::strcmp (plan.placements[p].row->role, "k") == 0)
                kickRow = plan.placements[p].lane;
        report (kickRow >= 0, "the main kick got placed");
        if (kickRow >= 0)
        {
            const char* src = "X...X...X...X...";
            bool exact = true;
            for (int s = 0; s < 16; ++s)
            {
                const float want = GenrePatterns::velocityFor (GenrePatterns::accentFor (src[s]));
                if (plan.velocity[kickRow][s] != want)
                    exact = false;
            }
            report (exact, "the kick lane carries the template's own four-on-the-floor");
        }

        // Lanes the template did not use must come back silent, so generating replaces the
        // grid instead of overdubbing whatever was there.
        int untouchedNoisy = 0;
        for (int l = 0; l < maxLanes; ++l)
        {
            if (seen[l])
                continue;
            for (int s = 0; s < GenrePatterns::numSteps; ++s)
                if (plan.velocity[l][s] != 0.0f)
                    ++untouchedNoisy;
        }
        reportInt (untouchedNoisy, 0, "unplaced lanes are fully silent");

        // --- partial kits: the rule is "onto lanes that have a sample", not "onto pad 1".
        // Kick and snare only: nothing else may be invented, and the hats the genre wants
        // have to show up as dropped rather than as a silent lane the user blames the kit for.
        {
            static const char* const twoLane[] = { "kick.wav", "snare.wav", nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
            LaneInfo lanes2[maxLanes];
            fillKit (twoLane, lanes2);
            const auto p2 = makePlan (genre, 0, &lanes2[0], maxLanes);
            int loaded = 0;
            for (int l = 0; l < maxLanes; ++l)
                if (lanes2[l].hasSample)
                    ++loaded;
            reportInt (loaded, 2, "the two-lane kit really has two samples");
            report (p2.numPlaced <= 2, "a two-sample kit places at most two rows");
            report (p2.numPlaced + p2.dropped[0] + p2.dropped[1] + p2.dropped[2] + p2.dropped[3]
                    == p2.totalRows, "the two-lane plan accounts for every row");
            report (p2.dropped[3] > 0, "the missing hats are reported, not hidden");
        }

        // Five lanes: kick, snare, hat, clap plus one the user renamed to something that
        // says nothing. That last one is free real estate and should absorb a row nothing
        // else is named for. The neutral strip names matter -- every default name classifies,
        // so a lane only becomes unnamed once the user has renamed it.
        {
            static const char* const fiveLane[] = { "kick.wav", "snare.wav", "hat.wav", "clap.wav",
                "weird_texture.wav", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
            static const char* const fiveStrips[] = { "Kick", "Snare", "Hat", "Clap", "Lane",
                "Lane", "Lane", "Lane", "Lane", "Lane", "Lane", "Lane" };
            LaneInfo lanes5[maxLanes];
            fillNamedKit (fiveLane, fiveStrips, lanes5);
            const auto p5 = makePlan (genre, 0, &lanes5[0], maxLanes);
            report (familyOfLane (lanes5[4]) == nullptr, "a renamed lane with an unkind name classifies to nothing");
            bool unnamedUsed = false;
            for (int p = 0; p < p5.numPlaced; ++p)
                if (p5.placements[p].lane == 4 && ! p5.placements[p].byName)
                    unnamedUsed = true;
            report (unnamedUsed, "an unnamed sample takes a row nothing else is named for");
            report (p5.numPlaced <= 5, "five samples cap the plan at five rows");

            // A named lane must never be stolen from its own family: the kick stays the kick,
            // even if the genre has more kick rows than the kit has kick lanes.
            bool kickStillKick = false;
            for (int p = 0; p < p5.numPlaced; ++p)
                if (p5.placements[p].lane == 0 && p5.placements[p].byName
                    && p5.placements[p].row != nullptr && p5.placements[p].row->family == GenrePatterns::Family::kick)
                    kickStillKick = true;
            report (kickStillKick, "the kick lane keeps the kick row");
        }

        // Twelve lanes of one family: the extra rows spill onto unnamed lanes only in name,
        // so a kit of twelve kicks can only ever hold twelve kick rows and drops the rest.
        {
            LaneInfo mono[maxLanes];
            for (int l = 0; l < maxLanes; ++l)
            {
                mono[l].hasSample   = true;
                mono[l].sampleName  = "kick.wav";
                mono[l].channelName = "Lane";
            }
            const auto pm = makePlan (genre, 0, &mono[0], maxLanes);
            int nonKickPlaced = 0;
            for (int p = 0; p < pm.numPlaced; ++p)
                if (pm.placements[p].row != nullptr && pm.placements[p].row->family != GenrePatterns::Family::kick)
                    ++nonKickPlaced;
            reportInt (nonKickPlaced, 0, "a one-family kit is never forced into the wrong family");
            report (pm.numPlaced == 1, "one kick lane holds one row, however many the genre asks for");
        }
    }

    //======================================================================
    std::printf ("\n[4] an empty kit generates silence, not a guess\n");
    {
        LaneInfo empty[maxLanes];
        for (int l = 0; l < maxLanes; ++l)
            empty[l].hasSample = false;

        const auto& genre = GenrePatterns::genres[0];
        const auto plan = makePlan (genre, 0, &empty[0], maxLanes);

        reportInt (plan.numPlaced, 0, "nothing is placed on an empty kit");
        reportInt (plan.dropped[0] + plan.dropped[1] + plan.dropped[2] + plan.dropped[3],
                   plan.totalRows, "every row is reported as dropped");

        int nonSilent = 0;
        for (int l = 0; l < maxLanes; ++l)
            for (int s = 0; s < GenrePatterns::numSteps; ++s)
                if (plan.velocity[l][s] != 0.0f)
                    ++nonSilent;
        reportInt (nonSilent, 0, "the grid is all silence");
    }

    //======================================================================
    std::printf ("\n[5] the name classifier reads what a person would call the file\n");
    {
        using F = GenrePatterns::Family;
        auto fam = [] (const char* n) {
            const auto* f = familyOfName (n);
            return f == nullptr ? -1 : (int) *f;
        };
        const int kick  = (int) F::kick,  snare = (int) F::snare;
        const int clap  = (int) F::clap,  hat   = (int) F::hat;

        reportInt (fam ("808kick.wav"),  kick,  "808kick is a kick");
        reportInt (fam ("KICK.wav"),      kick,  "upper case still reads");
        reportInt (fam ("kick_layer2.wav"), kick, "a kick layer is a kick");
        reportInt (fam ("BD_hard.wav"),   kick,  "BD is a kick");
        reportInt (fam ("sub_bass.wav"),  kick,  "a sub is a kick");
        reportInt (fam ("tom_low.wav"),   kick,  "a tom is a tuned membrane, so the kick lane");
        reportInt (fam ("conga_hi.wav"),  kick,  "a conga joins the membranes");

        reportInt (fam ("snare.wav"),     snare, "snare is a snare");
        reportInt (fam ("ghost_note.wav"), snare, "a ghost note is a snare");
        reportInt (fam ("sd.wav"),        snare, "sd is a snare");

        reportInt (fam ("clap.wav"),      clap,  "clap is a clap");
        reportInt (fam ("rimshot.wav"),   clap,  "a rimshot is a clap-family transient");
        reportInt (fam ("snap.wav"),      clap,  "a snap is a clap");

        reportInt (fam ("closed_hat.wav"), hat,  "a closed hat is a hat");
        reportInt (fam ("open_hat.wav"),   hat,  "an open hat is a hat");
        reportInt (fam ("ride.wav"),       hat,  "a ride is a hat");
        reportInt (fam ("crash.wav"),      hat,  "a crash is a hat");
        reportInt (fam ("shaker.wav"),     hat,  "a shaker is a hat");
        reportInt (fam ("cowbell.wav"),    hat,  "a cowbell is a hat");

        reportInt (fam (""),               -1,   "an empty name says nothing");
        reportInt (fam ("weird_texture.wav"), -1, "a texture claims no family");
        reportInt (fam ("field_recording.wav"), -1, "a field recording claims no family");

        // Order matters where a name could be two things; the hierarchy is the tiebreak.
        reportInt (fam ("kick_snare_combo.wav"), kick, "kick wins the tie, per the documented order");
        reportInt (fam ("hat_perc.wav"),    hat,  "hat is checked after clap and reads here as a hat");

        // A file name outranks the strip it sits on: Rim.wav on the Hat strip is a rim.
        LaneInfo lane;
        lane.hasSample   = true;
        lane.sampleName  = "rim.wav";
        lane.channelName = "Hat";
        report (*familyOfLane (lane) == F::clap, "a file name beats a strip name");

        lane.sampleName = "";
        report (*familyOfLane (lane) == F::hat, "with no file name, the strip name decides");
    }

    //======================================================================
    std::printf ("\n[6] a seed replays\n");
    {
        static const char* const fullKit[] = {
            "808kick.wav", "snare.wav", "closed_hat.wav", "clap.wav", "tom.wav", "rim.wav",
            "cowbell.wav", "conga.wav", "shaker.wav", "crash.wav", "ride.wav", "perc.wav" };
        LaneInfo lanes[maxLanes];
        fillKit (fullKit, lanes);

        int differing = 0, wrongCore = 0;
        for (int g = 0; g < GenrePatterns::numGenres; ++g)
        {
            const auto& genre = GenrePatterns::genres[g];
            for (unsigned int seed = 0; seed < 24; ++seed)
            {
                const int core = coreFor (genre, seed);
                if (core < 0 || core >= genre.numCores)
                    ++wrongCore;
                const auto a = makePlan (genre, core, &lanes[0], maxLanes);
                const auto b = makePlan (genre, core, &lanes[0], maxLanes);
                if (std::memcmp (a.velocity, b.velocity, sizeof (a.velocity)) != 0
                    || a.numPlaced != b.numPlaced)
                    ++differing;
            }
        }
        reportInt (wrongCore, 0, "coreFor always names a core the genre has");
        reportInt (differing, 0, "the same seed and kit give the identical grid, for all 29 genres");

        // Seed 0 is the documented first variant, not a random one.
        const auto& genre = GenrePatterns::genres[0];
        reportInt (coreFor (genre, 0u), 0, "seed 0 is the genre's first pattern");
        reportInt (coreFor (genre, (unsigned int) genre.numCores), 0, "a full cycle wraps to the first");
        reportInt (coreFor (genre, (unsigned int) genre.numCores + 3u), 3, "and lands where it should");

        // Rolling must reach every variant the table carries -- otherwise a genre with four
        // cores only ever shows one of them and the port is wasted.
        bool coversAll = true;
        for (int g = 0; g < GenrePatterns::numGenres && coversAll; ++g)
        {
            const auto& g2 = GenrePatterns::genres[g];
            bool seenCore[8] {};
            unsigned int s = (unsigned int) g + 1u;
            for (int i = 0; i < 200; ++i)
            {
                seenCore[coreFor (g2, s)] = true;
                s = nextSeed (s);
            }
            for (int c = 0; c < g2.numCores; ++c)
                if (! seenCore[c])
                    coversAll = false;
        }
        report (coversAll, "rolling reaches every variant of every genre");
    }

    //======================================================================
    std::printf ("\n[7] a roll changes the lane it picks\n");
    {
        int hitsCurrent = 0, outOfRange = 0, neverPicked = 0;
        for (int pool = 1; pool <= 12; ++pool)
        {
            for (int cur = -1; cur < pool; ++cur)
            {
                bool picked[12] {};
                for (unsigned int seed = 1; seed < 400; ++seed)
                {
                    const int i = pickFromPool (pool, cur, seed);
                    if (i < 0 || i >= pool)
                        ++outOfRange;
                    else
                        picked[i] = true;
                    if (pool > 1 && i == cur)
                        ++hitsCurrent;
                }
                for (int i = 0; i < pool; ++i)
                {
                    // With more than one file, and enough rolls, every entry must come up at
                    // least once -- and when cur is the only other choice it is legitimately
                    // reachable only through the no-current path.
                    const bool reachable = (pool > 1 && cur >= 0 && cur != i)
                                           || cur < 0 || cur >= pool;
                    if (reachable && ! picked[i])
                        ++neverPicked;
                }
            }
        }
        reportInt (outOfRange, 0, "a pick always addresses an entry that exists");
        reportInt (hitsCurrent, 0, "a roll never re-picks the file already loaded");
        reportInt (neverPicked, 0, "every file in a pool is reachable");

        reportInt (pickFromPool (0, -1, 12345u), -1, "an empty pool has nothing to pick");
        reportInt (pickFromPool (1, 0, 12345u), 0, "a one-file pool returns it");
        reportInt (pickFromPool (1, -1, 12345u), 0, "even before anything is loaded");

        // Determinism for the same triple, so a session can explain its own last pick.
        int unstable = 0;
        for (unsigned int seed = 1; seed < 500; ++seed)
            if (pickFromPool (9, 3, seed) != pickFromPool (9, 3, seed))
                ++unstable;
        reportInt (unstable, 0, "the same (pool, index, seed) always gives the same pick");

        // nextSeed must not stall: a counter that lands on 0 and stays there would freeze
        // every roll in every session at one pattern.
        unsigned int s = 1u;
        bool repeated = false, zeroed = false;
        unsigned int seenSeeds[1024];
        int numSeen = 0;
        for (int i = 0; i < 1024; ++i)
        {
            s = nextSeed (s);
            if (s == 0u)
                zeroed = true;
            for (int j = 0; j < numSeen; ++j)
                if (seenSeeds[j] == s)
                    repeated = true;
            seenSeeds[numSeen++] = s;
        }
        report (! zeroed,   "the roll counter never lands on zero");
        report (! repeated, "a thousand rolls are a thousand different seeds");
        report (nextSeed (0u) != 0u, "a zero seed still advances");
    }

    std::printf ("\n%d checks, %d problems\n", checks, problems);
    return problems == 0 ? 0 : 1;
}
