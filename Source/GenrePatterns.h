// GenrePatterns.h -- the genre grid templates, ported from the VRAC+ kit.
//
// GENERATED, do not hand-edit: out/genre-port/emit-genre-patterns.cjs writes this from
// drumgen/index.html (GENRES, GENRE_ALIAS, GENRE_VEL, INST) and refuses to install the file
// unless every 16-step row string matches the source and the table checksums agree.
//
//   29 genres, 116 variants ("cores"), 533 rows, all 16 steps.
//   sha256 over the canonical table: 5d5bbbef824dd118c46b93f26387d86ed4ab2c612ce7e67af95433976fe25c12
//   fnv1a64 over the canonical table: 0x9d7b42919b918620 -- the value
//   Tests/PatternPlanTest.cpp recomputes from this header; it is the same table, so a
//   hand-edit here goes red in the test suite without anyone remembering to run this script.
//   The canonical text is, for every row in table order with no separator other than '\n':
//       <genre id>|<core index>|<role>|<family name>|<16-char pattern>
//   family name is the lowercase word: kick, snare, clap, hat.
//
// A row is one drum part, in the family the pad belongs to, written in the same four
// characters the source uses: '.' silent, 'o' a ghost, 'x' a push, 'X' an accent. The three
// velocities are 0.48, 0.72 and 1 and come from GENRE_VEL, so a ported grid
// hits the same dynamics here that it hits in the web app.
//
// The bpm and swingPercent fields describe the genre and are deliberately NOT applied by the
// generator: in a VST3 the host owns the tempo, and swing is a control the user set. They are
// carried so the UI can name what the style suggests.
#pragma once

namespace GenrePatterns
{
    enum class Family : int { kick = 0, snare = 1, clap = 2, hat = 3 };

    /** What one cell of a template row means, and the level it asks for. */
    enum class Accent : int { silent = 0, ghost = 1, push = 2, full = 3 };

    inline constexpr int numSteps = 16;
    inline constexpr float ghostVelocity = 0.48f;
    inline constexpr float pushVelocity  = 0.72f;
    inline constexpr float fullVelocity  = 1.00f;

    inline constexpr float velocityFor (const Accent a) noexcept
    {
        switch (a)
        {
            case Accent::ghost: return ghostVelocity;
            case Accent::push:  return pushVelocity;
            case Accent::full:  return fullVelocity;
            case Accent::silent: break;
        }
        return 0.0f;
    }

    /** One character of a template row, in the source's own notation. */
    inline constexpr Accent accentFor (const char c) noexcept
    {
        switch (c)
        {
            case 'X': return Accent::full;
            case 'x': return Accent::push;
            case 'o': return Accent::ghost;
            default: break;
        }
        return Accent::silent;
    }

    /** The source's own word for a pad family -- the same string the port was checksummed over. */
    inline constexpr const char* const familyNames[] = { "kick", "snare", "clap", "hat" };

    struct Row
    {
        const char* role;      // the source's pad key, kept so a diff says which part moved
        const char* pattern;   // 16 characters of . x o X
        Family      family;
    };

    struct Core { const Row* rows; int numRows; };

    struct Genre
    {
        const char* id;           // key in the source table, lowercase, no spaces
        const char* name;         // what the button shows
        int         bpm;
        int         swingPercent; // the source's 0..100 slider value, descriptive only
        const Core* cores;
        int         numCores;
    };

    inline constexpr Row technoCore0[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "c", "....X.......X...", Family::clap   },
        { "m", "......o...o.....", Family::hat    },
    };

    inline constexpr Row technoCore1[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "k2", "..x.....x..x....", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "h", ".x.x.x.x.xx.x.x.", Family::hat    },
        { "c", "....X.......X...", Family::clap   },
        { "m2", "..........x.....", Family::hat    },
    };

    inline constexpr Row technoCore2[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "......x.......x.", Family::hat    },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "a2", "..........x.....", Family::hat    },
        { "c3", "............X...", Family::clap   },
    };

    inline constexpr Row technoCore3[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "k2", "...............x", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "h", ".x.x.x.o.x.x.x.x", Family::hat    },
        { "c", "......X.......X.", Family::clap   },
        { "m3", "....o.......o...", Family::hat    },
    };

    inline constexpr Core techno[] =
    {
        { technoCore0, 5 },
        { technoCore1, 6 },
        { technoCore2, 5 },
        { technoCore3, 6 },
    };

    inline constexpr Row houseCore0[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "c", "....X.......X...", Family::clap   },
    };

    inline constexpr Row houseCore1[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "k2", "..........x.....", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "s", "....X.......X...", Family::snare  },
        { "h", ".x.xx.x..x.xx.x.", Family::hat    },
    };

    inline constexpr Row houseCore2[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "......x...x...x.", Family::hat    },
        { "c2", "....X.......X...", Family::clap   },
        { "a2", "..x.......x.....", Family::hat    },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Row houseCore3[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "k2", "....x...........", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "s2", "....X.......X...", Family::snare  },
        { "m", "...x......x.....", Family::hat    },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Core house[] =
    {
        { houseCore0, 4 },
        { houseCore1, 5 },
        { houseCore2, 5 },
        { houseCore3, 6 },
    };

    inline constexpr Row jungleCore0[] =
    {
        { "k", "X.....X...X.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "s2", ".......x..x.....", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Row jungleCore1[] =
    {
        { "k", "X..x..X..x..X...", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "s2", "..x..x...x..x...", Family::snare  },
        { "m", ".....x.....x....", Family::hat    },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Row jungleCore2[] =
    {
        { "k", "X..x.....X.x....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "s2", "..x.......x.....", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "c", "....o.......o...", Family::clap   },
    };

    inline constexpr Row jungleCore3[] =
    {
        { "k", "X......xX.....x.", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "s2", "....x.x...x.x...", Family::snare  },
        { "o", "..............x.", Family::hat    },
        { "m2", "...x.........x..", Family::hat    },
    };

    inline constexpr Core jungle[] =
    {
        { jungleCore0, 4 },
        { jungleCore1, 5 },
        { jungleCore2, 5 },
        { jungleCore3, 5 },
    };

    inline constexpr Row hiphopCore0[] =
    {
        { "k", "X......x..X.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
        { "c", "....o.......o...", Family::clap   },
    };

    inline constexpr Row hiphopCore1[] =
    {
        { "k", "X..x...x..X.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
        { "m", "..............x.", Family::hat    },
    };

    inline constexpr Row hiphopCore2[] =
    {
        { "k", "X.....x...X..x..", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
        { "a", "..x.......x.....", Family::hat    },
    };

    inline constexpr Row hiphopCore3[] =
    {
        { "k", "X..x..x...X.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
        { "c3", "..............X.", Family::clap   },
    };

    inline constexpr Core hiphop[] =
    {
        { hiphopCore0, 4 },
        { hiphopCore1, 4 },
        { hiphopCore2, 4 },
        { hiphopCore3, 4 },
    };

    inline constexpr Row trapCore0[] =
    {
        { "k", "X.......X.......", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "c2", "........o.......", Family::clap   },
        { "h", "x.x.x.x.x.x.x.xx", Family::hat    },
    };

    inline constexpr Row trapCore1[] =
    {
        { "k", "X.....x.X...x...", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Row trapCore2[] =
    {
        { "k", "X...x...X....x..", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "c3", "........o.......", Family::clap   },
        { "o", "..........x.....", Family::hat    },
        { "h", "x.x.x.xxx.x.x.x.", Family::hat    },
    };

    inline constexpr Row trapCore3[] =
    {
        { "k", "X.......X..x....", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "c4", "........o.......", Family::clap   },
        { "h", "x.x.x.x.x.xxx.x.", Family::hat    },
        { "a2", "......x.......x.", Family::hat    },
    };

    inline constexpr Core trap[] =
    {
        { trapCore0, 4 },
        { trapCore1, 3 },
        { trapCore2, 5 },
        { trapCore3, 5 },
    };

    inline constexpr Row electroCore0[] =
    {
        { "k", "X..x..X...x.X...", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "m", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "c4", "....o.......o...", Family::clap   },
    };

    inline constexpr Row electroCore1[] =
    {
        { "k", "X.....X..xX.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "m2", "..x...x...x...x.", Family::hat    },
        { "a2", "........o.......", Family::hat    },
    };

    inline constexpr Row electroCore2[] =
    {
        { "k", "X..X..X...X..X..", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "m3", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "o", "......x.......x.", Family::hat    },
    };

    inline constexpr Row electroCore3[] =
    {
        { "k", "X...X..x..X...x.", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "m", "x...x...x...x...", Family::hat    },
        { "c5", "..............X.", Family::clap   },
    };

    inline constexpr Core electro[] =
    {
        { electroCore0, 4 },
        { electroCore1, 4 },
        { electroCore2, 4 },
        { electroCore3, 4 },
    };

    inline constexpr Row ukgCore0[] =
    {
        { "k", "X.........x.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c2", ".......x.......x", Family::clap   },
        { "m", "..............o.", Family::hat    },
    };

    inline constexpr Row ukgCore1[] =
    {
        { "k", "X......x..X.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
        { "o", "..x...x...x...x.", Family::hat    },
        { "a2", "..x.......x.....", Family::hat    },
        { "c3", "..............X.", Family::clap   },
    };

    inline constexpr Row ukgCore2[] =
    {
        { "k", "X..x......X..x..", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "a2", "..x.......x.....", Family::hat    },
        { "c4", "...x......x.....", Family::clap   },
        { "m2", "......x.......x.", Family::hat    },
    };

    inline constexpr Row ukgCore3[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c5", "............X...", Family::clap   },
        { "m3", "..x...x...x...x.", Family::hat    },
    };

    inline constexpr Core ukg[] =
    {
        { ukgCore0, 6 },
        { ukgCore1, 6 },
        { ukgCore2, 6 },
        { ukgCore3, 6 },
    };

    inline constexpr Row dubstepCore0[] =
    {
        { "k", "X.......X.......", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
        { "c2", "........o.......", Family::clap   },
        { "m", "..x.......x.....", Family::hat    },
    };

    inline constexpr Row dubstepCore1[] =
    {
        { "k", "X.....x.X...x...", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "h", "..x...x...x...x.", Family::hat    },
        { "o", "............x...", Family::hat    },
        { "c3", "........o.......", Family::clap   },
    };

    inline constexpr Row dubstepCore2[] =
    {
        { "k", "X...x...X.......", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "h", "x.x.x.x.x.x.x.xx", Family::hat    },
        { "c4", "........o.o.....", Family::clap   },
        { "m2", "....x.......x...", Family::hat    },
    };

    inline constexpr Row dubstepCore3[] =
    {
        { "k", "X.......X.....x.", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "h", "x.x.x.x.x.xxx.x.", Family::hat    },
        { "o", "..........x.....", Family::hat    },
        { "c5", "..............X.", Family::clap   },
    };

    inline constexpr Core dubstep[] =
    {
        { dubstepCore0, 5 },
        { dubstepCore1, 5 },
        { dubstepCore2, 5 },
        { dubstepCore3, 5 },
    };

    inline constexpr Row tranceCore0[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c", "....X.......X...", Family::clap   },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "m", "......x.......x.", Family::hat    },
    };

    inline constexpr Row tranceCore1[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "s2", "....X.......X...", Family::snare  },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
        { "c2", "..............x.", Family::clap   },
        { "a2", "..x.......x.....", Family::hat    },
    };

    inline constexpr Row tranceCore2[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c", "....X.......X...", Family::clap   },
        { "m2", "x..x..x...x..x..", Family::hat    },
        { "a", "..............x.", Family::hat    },
    };

    inline constexpr Row tranceCore3[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c3", "....X.......X...", Family::clap   },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "m3", "....x...........", Family::hat    },
    };

    inline constexpr Core trance[] =
    {
        { tranceCore0, 5 },
        { tranceCore1, 6 },
        { tranceCore2, 5 },
        { tranceCore3, 5 },
    };

    inline constexpr Row teknoCore0[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c", "....X.......X...", Family::clap   },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
        { "m", "......x...x.....", Family::hat    },
    };

    inline constexpr Row teknoCore1[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c2", "....x.......x...", Family::clap   },
        { "m2", "..x...x...x...x.", Family::hat    },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Row teknoCore2[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c3", "....X.......X...", Family::clap   },
        { "m3", "x...x.x.x...x.x.", Family::hat    },
        { "a2", "......x.......x.", Family::hat    },
    };

    inline constexpr Row teknoCore3[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c4", "....X.......X...", Family::clap   },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
        { "m", "....o...o...o...", Family::hat    },
    };

    inline constexpr Core tekno[] =
    {
        { teknoCore0, 5 },
        { teknoCore1, 5 },
        { teknoCore2, 5 },
        { teknoCore3, 5 },
    };

    inline constexpr Row breakbeatCore0[] =
    {
        { "k", "X.....X...X.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
        { "o", "..............x.", Family::hat    },
        { "m", "..x.......x.....", Family::hat    },
    };

    inline constexpr Row breakbeatCore1[] =
    {
        { "k", "X..x..X..x..X...", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "s2", "..x..x...x..x...", Family::snare  },
        { "c2", ".......x........", Family::clap   },
    };

    inline constexpr Row breakbeatCore2[] =
    {
        { "k", "X...x...X...x...", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", "x.x.x.x.x.x.x.xx", Family::hat    },
        { "o", "..........x.....", Family::hat    },
        { "c3", "....o.......o...", Family::clap   },
    };

    inline constexpr Row breakbeatCore3[] =
    {
        { "k", "X......xX.....x.", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "s2", "....x.x...x.x...", Family::snare  },
        { "o", "..............x.", Family::hat    },
        { "m2", "...x.........x..", Family::hat    },
    };

    inline constexpr Core breakbeat[] =
    {
        { breakbeatCore0, 5 },
        { breakbeatCore1, 5 },
        { breakbeatCore2, 5 },
        { breakbeatCore3, 5 },
    };

    inline constexpr Row hyperpopCore0[] =
    {
        { "k", "X.......X.......", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "h", "x.x.x.x.x.x.x.xx", Family::hat    },
        { "c2", "........o.......", Family::clap   },
        { "a2", "......x.......x.", Family::hat    },
    };

    inline constexpr Row hyperpopCore1[] =
    {
        { "k", "X.....x.X...x...", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "c3", "....o.......o...", Family::clap   },
        { "m", "..x.......x.....", Family::hat    },
    };

    inline constexpr Row hyperpopCore2[] =
    {
        { "k", "X...x...X....x..", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "h", "x.x.x.xxx.x.x.x.", Family::hat    },
        { "c4", "........o.......", Family::clap   },
        { "a", "..............x.", Family::hat    },
    };

    inline constexpr Row hyperpopCore3[] =
    {
        { "k", "X.......X..x....", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "c5", "........o.......", Family::clap   },
        { "h", "x.x.x.x.x.xxx.x.", Family::hat    },
        { "m3", "......x.......x.", Family::hat    },
    };

    inline constexpr Core hyperpop[] =
    {
        { hyperpopCore0, 5 },
        { hyperpopCore1, 5 },
        { hyperpopCore2, 5 },
        { hyperpopCore3, 5 },
    };

    inline constexpr Row electropopCore0[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c", "....X.......X...", Family::clap   },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "m", "...x......x.....", Family::hat    },
    };

    inline constexpr Row electropopCore1[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "k2", "..........x.....", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "s2", "....X.......X...", Family::snare  },
        { "c2", "....X.......X...", Family::clap   },
        { "a2", "..x.......x.....", Family::hat    },
    };

    inline constexpr Row electropopCore2[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "......x...x...x.", Family::hat    },
        { "c3", "....X.......X...", Family::clap   },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "m2", "....o.......o...", Family::hat    },
    };

    inline constexpr Row electropopCore3[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c4", "....X.......X...", Family::clap   },
        { "s", "....x.......x...", Family::snare  },
        { "a", "..............x.", Family::hat    },
    };

    inline constexpr Core electropop[] =
    {
        { electropopCore0, 5 },
        { electropopCore1, 6 },
        { electropopCore2, 5 },
        { electropopCore3, 5 },
    };

    inline constexpr Row bigroomCore0[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "c", "....X.......X...", Family::clap   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "m", "......x.......x.", Family::hat    },
    };

    inline constexpr Row bigroomCore1[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "s2", "....X.......X...", Family::snare  },
        { "c2", "....X.......X...", Family::clap   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
    };

    inline constexpr Row bigroomCore2[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "c3", "....X.......X...", Family::clap   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "m2", "..x...x...x...x.", Family::hat    },
        { "a2", "......o.......o.", Family::hat    },
    };

    inline constexpr Row bigroomCore3[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "k2", "..x.......x.....", Family::kick   },
        { "c4", "....X.......X...", Family::clap   },
        { "o", "......x...x...x.", Family::hat    },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Core bigroom[] =
    {
        { bigroomCore0, 5 },
        { bigroomCore1, 5 },
        { bigroomCore2, 5 },
        { bigroomCore3, 5 },
    };

    inline constexpr Row drumandbassCore0[] =
    {
        { "k", "X.........X.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", "..x...x...x...x.", Family::hat    },
        { "o", "..............x.", Family::hat    },
    };

    inline constexpr Row drumandbassCore1[] =
    {
        { "k", "X.....x.X.x.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "s2", ".......x..x.....", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Row drumandbassCore2[] =
    {
        { "k", "X.........X..x..", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "c", "............o...", Family::clap   },
        { "h", "..x.x.x...x.x.x.", Family::hat    },
        { "a2", ".........x......", Family::hat    },
    };

    inline constexpr Row drumandbassCore3[] =
    {
        { "k", "X.........X.....", Family::kick   },
        { "k2", "..x.......x.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "m", ".......x........", Family::hat    },
    };

    inline constexpr Core drumandbass[] =
    {
        { drumandbassCore0, 4 },
        { drumandbassCore1, 4 },
        { drumandbassCore2, 5 },
        { drumandbassCore3, 5 },
    };

    inline constexpr Row halftimeCore0[] =
    {
        { "k", "X...............", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "h", "..x...x...x...x.", Family::hat    },
    };

    inline constexpr Row halftimeCore1[] =
    {
        { "k", "X.......X.......", Family::kick   },
        { "k2", "............x...", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "h", "..x.x...x.x.....", Family::hat    },
    };

    inline constexpr Row halftimeCore2[] =
    {
        { "k", "X.......X.......", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "c", "........X.......", Family::clap   },
        { "o", "............x...", Family::hat    },
        { "h", "x...x...x...x...", Family::hat    },
    };

    inline constexpr Row halftimeCore3[] =
    {
        { "k", "X.........x.....", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "s2", "..............x.", Family::snare  },
        { "m2", "............x...", Family::hat    },
        { "h", "..x...x...x.x...", Family::hat    },
    };

    inline constexpr Core halftime[] =
    {
        { halftimeCore0, 3 },
        { halftimeCore1, 4 },
        { halftimeCore2, 5 },
        { halftimeCore3, 5 },
    };

    inline constexpr Row dubstep140Core0[] =
    {
        { "k", "X.......X.......", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "h", "..x...x...x...x.", Family::hat    },
        { "a2", ".......x........", Family::hat    },
    };

    inline constexpr Row dubstep140Core1[] =
    {
        { "k", "X...........x...", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "c", "........X.......", Family::clap   },
        { "h", "..x..x....x..x..", Family::hat    },
        { "m3", "............o...", Family::hat    },
    };

    inline constexpr Row dubstep140Core2[] =
    {
        { "k", "X.......X....x..", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "c2", "........X.......", Family::clap   },
        { "a", "...x.......x....", Family::hat    },
        { "o", "..............x.", Family::hat    },
    };

    inline constexpr Row dubstep140Core3[] =
    {
        { "k", "X......xX.......", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "s2", "............x...", Family::snare  },
        { "h", ".x.x.x...x.x.x..", Family::hat    },
        { "m", "......x.........", Family::hat    },
    };

    inline constexpr Core dubstep140[] =
    {
        { dubstep140Core0, 4 },
        { dubstep140Core1, 5 },
        { dubstep140Core2, 5 },
        { dubstep140Core3, 5 },
    };

    inline constexpr Row leftfieldCore0[] =
    {
        { "k", "X.........x.....", Family::kick   },
        { "s", ".....X.....X....", Family::snare  },
        { "h", "...x...x...x...x", Family::hat    },
    };

    inline constexpr Row leftfieldCore1[] =
    {
        { "k", "X......x..X.....", Family::kick   },
        { "c", ".....x.....x....", Family::clap   },
        { "m2", "..x.......x.....", Family::hat    },
        { "o", "............x...", Family::hat    },
    };

    inline constexpr Row leftfieldCore2[] =
    {
        { "k", "X...x.......X...", Family::kick   },
        { "s2", "....x.......x...", Family::snare  },
        { "h", "x.x...x.x.x...x.", Family::hat    },
        { "a2", ".......x........", Family::hat    },
    };

    inline constexpr Row leftfieldCore3[] =
    {
        { "k", "X.........X.....", Family::kick   },
        { "k2", "...x............", Family::kick   },
        { "c3", "....X.......X...", Family::clap   },
        { "m", ".....x.....x....", Family::hat    },
        { "o", "..............x.", Family::hat    },
    };

    inline constexpr Core leftfield[] =
    {
        { leftfieldCore0, 3 },
        { leftfieldCore1, 4 },
        { leftfieldCore2, 4 },
        { leftfieldCore3, 5 },
    };

    inline constexpr Row drillCore0[] =
    {
        { "k", "X.......X..x....", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "h", "..x.x.x...x.x.x.", Family::hat    },
    };

    inline constexpr Row drillCore1[] =
    {
        { "k", "X....x..X.......", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "s2", ".....x......x...", Family::snare  },
        { "h", "x...x...x...x...", Family::hat    },
    };

    inline constexpr Row drillCore2[] =
    {
        { "k", "X.....x.X....x..", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "c", "........X.......", Family::clap   },
        { "h", "..x..x....x..x..", Family::hat    },
        { "a2", ".......x........", Family::hat    },
    };

    inline constexpr Row drillCore3[] =
    {
        { "k", "X.........x..X..", Family::kick   },
        { "s", "........X.......", Family::snare  },
        { "s2", "..............x.", Family::snare  },
        { "h", "..x.x..x..x.x..x", Family::hat    },
    };

    inline constexpr Core drill[] =
    {
        { drillCore0, 3 },
        { drillCore1, 4 },
        { drillCore2, 5 },
        { drillCore3, 4 },
    };

    inline constexpr Row phonkCore0[] =
    {
        { "k", "X.....X...X.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "m", "........x.......", Family::hat    },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
    };

    inline constexpr Row phonkCore1[] =
    {
        { "k", "X.....X...X....x", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "m2", "....x.......x...", Family::hat    },
        { "c", "....o.......o...", Family::clap   },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
    };

    inline constexpr Row phonkCore2[] =
    {
        { "k", "X...x.X...X.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "m3", "..x...x...x...x.", Family::hat    },
        { "o", "..............x.", Family::hat    },
    };

    inline constexpr Row phonkCore3[] =
    {
        { "k", "X.....X..xX.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "c2", "....X.......X...", Family::clap   },
        { "m", "......x.......x.", Family::hat    },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
    };

    inline constexpr Core phonk[] =
    {
        { phonkCore0, 4 },
        { phonkCore1, 5 },
        { phonkCore2, 4 },
        { phonkCore3, 5 },
    };

    inline constexpr Row futurebassCore0[] =
    {
        { "k", "X.......X.......", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "a", "..x.......x.....", Family::hat    },
    };

    inline constexpr Row futurebassCore1[] =
    {
        { "k", "X.......X.......", Family::kick   },
        { "s2", "....X.......X...", Family::snare  },
        { "c", "....X.......X...", Family::clap   },
        { "h", "x.xxx.x.x.xxx.x.", Family::hat    },
        { "m2", "......o...o.....", Family::hat    },
    };

    inline constexpr Row futurebassCore2[] =
    {
        { "k", "X...x...X.......", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "a2", "..x...x...x...x.", Family::hat    },
        { "o", "..............x.", Family::hat    },
    };

    inline constexpr Row futurebassCore3[] =
    {
        { "k", "X.......X...x...", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "c2", "....x.......x...", Family::clap   },
        { "h", "..x.x.x...x.x.x.", Family::hat    },
        { "m3", "....x.......x...", Family::hat    },
    };

    inline constexpr Core futurebass[] =
    {
        { futurebassCore0, 4 },
        { futurebassCore1, 5 },
        { futurebassCore2, 5 },
        { futurebassCore3, 5 },
    };

    inline constexpr Row reggaetonCore0[] =
    {
        { "k", "X.......X.......", Family::kick   },
        { "s", "..x...x...x...x.", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Row reggaetonCore1[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "s", "..x...x...x...x.", Family::snare  },
        { "c", "..x...x...x...x.", Family::clap   },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Row reggaetonCore2[] =
    {
        { "k", "X........X......", Family::kick   },
        { "s", "..x...x...x...x.", Family::snare  },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
        { "a2", ".....x.....x....", Family::hat    },
    };

    inline constexpr Row reggaetonCore3[] =
    {
        { "k", "X..xX.....X..x..", Family::kick   },
        { "s", "..x...x...x...x.", Family::snare  },
        { "s2", "..o...o...o...o.", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Core reggaeton[] =
    {
        { reggaetonCore0, 3 },
        { reggaetonCore1, 4 },
        { reggaetonCore2, 4 },
        { reggaetonCore3, 4 },
    };

    inline constexpr Row amapianoCore0[] =
    {
        { "k", "X...........X...", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", "..x.x.x...x.x.x.", Family::hat    },
    };

    inline constexpr Row amapianoCore1[] =
    {
        { "k", "X......x..X.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "c", "....o.......o...", Family::clap   },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
    };

    inline constexpr Row amapianoCore2[] =
    {
        { "k", "X.......X.......", Family::kick   },
        { "k2", ".....x.....x....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", "..x.x.x...x.x.x.", Family::hat    },
    };

    inline constexpr Row amapianoCore3[] =
    {
        { "k", "X....x..X....x..", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "m2", ".......x........", Family::hat    },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Core amapiano[] =
    {
        { amapianoCore0, 3 },
        { amapianoCore1, 4 },
        { amapianoCore2, 4 },
        { amapianoCore3, 4 },
    };

    inline constexpr Row jerseyCore0[] =
    {
        { "k", "X.....X...X.....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "m", "..x...x..x...x..", Family::hat    },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
    };

    inline constexpr Row jerseyCore1[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "s2", "....X.......X...", Family::snare  },
        { "m2", "..x...x..x...x..", Family::hat    },
        { "c", "....x.......x...", Family::clap   },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
    };

    inline constexpr Row jerseyCore2[] =
    {
        { "k", "X.x.X.x.X.x.X.x.", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "m3", "..x...x..x...x..", Family::hat    },
        { "h", "..x...x...x...x.", Family::hat    },
    };

    inline constexpr Row jerseyCore3[] =
    {
        { "k", "X..x..X..x..X...", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "c2", "....o.......o...", Family::clap   },
        { "m", "..x...x..x...x..", Family::hat    },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
    };

    inline constexpr Core jersey[] =
    {
        { jerseyCore0, 4 },
        { jerseyCore1, 5 },
        { jerseyCore2, 4 },
        { jerseyCore3, 5 },
    };

    inline constexpr Row lofiCore0[] =
    {
        { "k", "X.......X.......", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", "..x.x...x.x.x...", Family::hat    },
        { "m", ".....x.....x....", Family::hat    },
    };

    inline constexpr Row lofiCore1[] =
    {
        { "k", "X..x..X.......x.", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
    };

    inline constexpr Row lofiCore2[] =
    {
        { "k", "X.......X..x....", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "c", "....o.......o...", Family::clap   },
        { "h", "..x...x...x...x.", Family::hat    },
    };

    inline constexpr Row lofiCore3[] =
    {
        { "k", "X....x..X.......", Family::kick   },
        { "s2", "....X.......X...", Family::snare  },
        { "m2", ".......x........", Family::hat    },
        { "o", "..............x.", Family::hat    },
        { "h", "..x.x.x...x.x...", Family::hat    },
    };

    inline constexpr Core lofi[] =
    {
        { lofiCore0, 4 },
        { lofiCore1, 3 },
        { lofiCore2, 4 },
        { lofiCore3, 5 },
    };

    inline constexpr Row psytranceCore0[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "h", "..x.x.x...x.x.x.", Family::hat    },
    };

    inline constexpr Row psytranceCore1[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "k2", "..x.......x.....", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "s", "....X.......X...", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Row psytranceCore2[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c", "....X.......X...", Family::clap   },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
    };

    inline constexpr Row psytranceCore3[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "s2", "..x..x..x..x..x.", Family::snare  },
        { "m", "......x.......x.", Family::hat    },
    };

    inline constexpr Core psytrance[] =
    {
        { psytranceCore0, 3 },
        { psytranceCore1, 5 },
        { psytranceCore2, 4 },
        { psytranceCore3, 4 },
    };

    inline constexpr Row basshouseCore0[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "s", "....X.......X...", Family::snare  },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Row basshouseCore1[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "k2", ".......x........", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c", "....X.......X...", Family::clap   },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
    };

    inline constexpr Row basshouseCore2[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "s2", "....X.......X...", Family::snare  },
        { "o", "..x...x...x...x.", Family::hat    },
        { "a2", "..x.......x.....", Family::hat    },
        { "m2", ".......o...o....", Family::hat    },
    };

    inline constexpr Row basshouseCore3[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c3", "....x.......x...", Family::clap   },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
        { "a", "..x....x..x.....", Family::hat    },
    };

    inline constexpr Core basshouse[] =
    {
        { basshouseCore0, 4 },
        { basshouseCore1, 5 },
        { basshouseCore2, 5 },
        { basshouseCore3, 5 },
    };

    inline constexpr Row hardstyleCore0[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "s", "....X.......X...", Family::snare  },
    };

    inline constexpr Row hardstyleCore1[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c", "....X.......X...", Family::clap   },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Row hardstyleCore2[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "s2", "....X.......X...", Family::snare  },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
    };

    inline constexpr Row hardstyleCore3[] =
    {
        { "k", "X...X...X...X...", Family::kick   },
        { "k2", "..x.......x.....", Family::kick   },
        { "o", "..x...x...x...x.", Family::hat    },
        { "c2", "....X.......X...", Family::clap   },
        { "m", ".......o...o....", Family::hat    },
    };

    inline constexpr Core hardstyle[] =
    {
        { hardstyleCore0, 3 },
        { hardstyleCore1, 4 },
        { hardstyleCore2, 4 },
        { hardstyleCore3, 5 },
    };

    inline constexpr Row electro808Core0[] =
    {
        { "k", "X........X......", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "m", "..x...x...x...x.", Family::hat    },
        { "h", "..x...x...x...x.", Family::hat    },
    };

    inline constexpr Row electro808Core1[] =
    {
        { "k", "X.....X..X......", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "c", "....X.......X...", Family::clap   },
        { "m2", "..x..x...x..x...", Family::hat    },
        { "h", ".x.x.x.x.x.x.x.x", Family::hat    },
    };

    inline constexpr Row electro808Core2[] =
    {
        { "k", "X......xX.......", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "m3", "..x...x...x...x.", Family::hat    },
        { "o", "..............x.", Family::hat    },
        { "a2", ".....x.....x....", Family::hat    },
    };

    inline constexpr Row electro808Core3[] =
    {
        { "k", "X..x..X.....x...", Family::kick   },
        { "s", "....X.......X...", Family::snare  },
        { "c2", "....o.......o...", Family::clap   },
        { "m", "..x...x...x...x.", Family::hat    },
        { "h", "x.x.x.x.x.x.x.x.", Family::hat    },
    };

    inline constexpr Core electro808[] =
    {
        { electro808Core0, 4 },
        { electro808Core1, 5 },
        { electro808Core2, 5 },
        { electro808Core3, 5 },
    };

    inline constexpr Genre genres[] =
    {
        { "techno", "Techno", 132, 8, techno, 4 },
        { "house", "House", 124, 20, house, 4 },
        { "jungle", "Breaks / Jungle", 172, 0, jungle, 4 },
        { "hiphop", "Hip-Hop", 90, 24, hiphop, 4 },
        { "trap", "Trap", 140, 0, trap, 4 },
        { "electro", "Electro", 128, 4, electro, 4 },
        { "ukg", "UK Garage", 132, 24, ukg, 4 },
        { "dubstep", "Dubstep", 140, 0, dubstep, 4 },
        { "trance", "Trance", 138, 0, trance, 4 },
        { "tekno", "Tekno", 155, 0, tekno, 4 },
        { "breakbeat", "Breakbeat", 134, 4, breakbeat, 4 },
        { "hyperpop", "Hyperpop", 158, 10, hyperpop, 4 },
        { "electropop", "Electro Pop", 118, 12, electropop, 4 },
        { "bigroom", "EDM / Big Room", 128, 0, bigroom, 4 },
        { "drumandbass", "Drum & Bass", 174, 0, drumandbass, 4 },
        { "halftime", "Halftime", 140, 0, halftime, 4 },
        { "dubstep140", "140 Dubstep", 140, 0, dubstep140, 4 },
        { "leftfield", "Leftfield Bass", 140, 6, leftfield, 4 },
        { "drill", "UK Drill", 140, 0, drill, 4 },
        { "phonk", "Phonk", 140, 0, phonk, 4 },
        { "futurebass", "Future Bass", 150, 0, futurebass, 4 },
        { "reggaeton", "Reggaeton", 96, 0, reggaeton, 4 },
        { "amapiano", "Amapiano", 112, 10, amapiano, 4 },
        { "jersey", "Jersey Club", 140, 0, jersey, 4 },
        { "lofi", "Lo-fi / Trip-hop", 85, 24, lofi, 4 },
        { "psytrance", "Psytrance", 142, 0, psytrance, 4 },
        { "basshouse", "Bass House", 128, 0, basshouse, 4 },
        { "hardstyle", "Hardstyle", 155, 0, hardstyle, 4 },
        { "electro808", "Electro 808", 120, 4, electro808, 4 },
    };

    inline constexpr int numGenres = 29;

    /** Every genre id, in the source's order, for the test that counts them. */
    inline constexpr const char* const genreIds[] = {
        "techno",
        "house",
        "jungle",
        "hiphop",
        "trap",
        "electro",
        "ukg",
        "dubstep",
        "trance",
        "tekno",
        "breakbeat",
        "hyperpop",
        "electropop",
        "bigroom",
        "drumandbass",
        "halftime",
        "dubstep140",
        "leftfield",
        "drill",
        "phonk",
        "futurebass",
        "reggaeton",
        "amapiano",
        "jersey",
        "lofi",
        "psytrance",
        "basshouse",
        "hardstyle",
        "electro808"
    };
}
