/* The clock source's adjustable period (examples/clock).

   The plugin is a single translation unit, so this test includes it and drives
   exactly what the game drives: clockLogic as the host calls it per simulation
   step, and renderClock as the host calls it per board frame with a draw API.
   Nothing here needs the game - only the pieces the game would hand over.

   What is covered:

     - the period comes out of the instance's own configuration, and the
       registered default (and a configuration that predates the field) is the
       classic once-per-cycle square wave;
     - the counter lives in the component's simulation state, so RESET restarts
       the phase and REFRESH reports without advancing;
     - two instances hold independent periods;
     - the click ladder steps and wraps, including from an off-ladder value;
     - the face draws the period that the instance is set to, in one, two and
       three digits, inside the painted panel.

   Not covered here (needs the real board): the click itself, i.e. whether the
   game's selection set answers for one of these instances when the player
   clicks it, and whether the configuration write reaches the record.  Both
   paths are the same ones the interactive pair already exercises in game. */

#include "../examples/clock/plugin.cpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    ++g_failures;
    std::printf("FAIL  %s\n", what.c_str());
}

/* One instance, driven the way the host drives it: a stable fake id, its own
   state words and its own configuration bytes. */
struct Instance {
    uint64_t outputs[1] = {0};
    uint64_t state[2] = {0, 0};
    uint8_t config[sizeof(ClockConfig)] = {1, 0};

    TCLogicIOV2 io(uint32_t phase, uint64_t id) {
        TCLogicIOV2 io{};
        io.size = sizeof(io);
        io.version = TC_LOGIC_IO_V2_VERSION_1;
        io.phase = phase;
        io.instance_id = id;
        io.cycle = 0;
        io.output_count = 1;
        io.outputs = outputs;
        io.state = state;
        io.state_words = 2;
        io.config = config;
        io.config_size = sizeof(config);
        io.config_schema = kClockConfigSchema;
        return io;
    }

    uint64_t cycle(uint64_t id) {
        TCLogicIOV2 io = this->io(TC_LOGIC_CYCLE, id);
        clockLogic(&io);
        return outputs[0];
    }

    uint64_t refresh(uint64_t id) {
        TCLogicIOV2 io = this->io(TC_LOGIC_REFRESH, id);
        clockLogic(&io);
        return outputs[0];
    }

    void reset(uint64_t id) {
        TCLogicIOV2 io = this->io(TC_LOGIC_RESET, id);
        clockLogic(&io);
    }

    void setPeriod(uint8_t period) { config[0] = period; }

    std::vector<uint64_t> run(uint64_t id, int cycles) {
        std::vector<uint64_t> values;
        values.reserve(static_cast<size_t>(cycles));
        for (int i = 0; i < cycles; ++i) values.push_back(cycle(id));
        return values;
    }
};

/* The wave a period-`period` clock must produce: the output starts low and
   flips once every `period` cycles, so after `n` cycles it has flipped
   floor(n / period) times. */
std::vector<uint64_t> expectedWave(uint8_t period, int cycles) {
    std::vector<uint64_t> values;
    values.reserve(static_cast<size_t>(cycles));
    for (int n = 1; n <= cycles; ++n)
        values.push_back(static_cast<uint64_t>((n / period) & 1));
    return values;
}

std::string waveText(const std::vector<uint64_t>& values) {
    std::string text;
    for (const uint64_t value : values) text += value ? '1' : '0';
    return text;
}

std::string describe(const std::vector<uint64_t>& values) {
    return "'" + waveText(values) + "'";
}

void checkWave(uint8_t period, int cycles, const std::string& what) {
    Instance instance;
    instance.setPeriod(period);
    const std::vector<uint64_t> actual = instance.run(0x1001, cycles);
    const std::vector<uint64_t> expected = expectedWave(period, cycles);
    if (actual == expected) {
        std::printf("ok    %s: period %u -> %s\n", what.c_str(),
                    static_cast<unsigned>(period), describe(actual).c_str());
        return;
    }
    check(false, what + ": period " + std::to_string(period) + " produced " +
                     describe(actual) + ", expected " + describe(expected));
}

void testWaves() {
    /* 1 has to reproduce the original build exactly: one flip per cycle. */
    checkWave(1, 8, "once per cycle");
    checkWave(2, 8, "every second cycle");
    checkWave(3, 12, "every third cycle");
    checkWave(4, 12, "every fourth cycle");
    checkWave(5, 10, "every fifth cycle");
    checkWave(16, 33, "every sixteenth cycle");
    checkWave(100, 201, "the slowest step of the ladder");

    /* A configuration that predates the field, or one the host refused, reads as
       0 bytes here: the clock must fall back to the default, not to a period of
       zero (which would never flip) and not to a crash. */
    Instance legacy;
    legacy.config[0] = 0;
    const std::vector<uint64_t> values = legacy.run(0x1002, 4);
    check(values == expectedWave(1, 4),
          "an empty configuration falls back to the once-per-cycle default: " +
              describe(values));
}

void testResetAndRefresh() {
    Instance instance;
    instance.setPeriod(3);
    const std::vector<uint64_t> first = instance.run(0x2001, 4);
    check(first == expectedWave(3, 4), "run before reset: " + describe(first));
    check(instance.state[0] == 1 && instance.state[1] == 1,
          "after four cycles of a period-3 clock the state is level 1 holding 1");

    instance.reset(0x2001);
    check(instance.state[0] == 0 && instance.state[1] == 0,
          "RESET clears both the level and the cycles held");
    check(instance.outputs[0] == 0, "RESET reports a low output");
    const std::vector<uint64_t> second = instance.run(0x2001, 4);
    check(second == first, "the phase restarts after RESET: " + describe(second));

    Instance refresh;
    refresh.setPeriod(2);
    (void)refresh.cycle(0x2002);
    const uint64_t peeks[] = {refresh.refresh(0x2002), refresh.refresh(0x2002),
                              refresh.refresh(0x2002)};
    check(peeks[0] == 0 && peeks[1] == 0 && peeks[2] == 0,
          "REFRESH reports the current level");
    check(refresh.state[1] == 1,
          "REFRESH does not advance the cycle counter");
    check(refresh.cycle(0x2002) == 1,
          "the cycle after a REFRESH still flips on time");
    check(refresh.refresh(0x2002) == 1,
          "REFRESH reports the level the flip produced");
    check(refresh.cycle(0x2002) == 1 && refresh.cycle(0x2002) == 0,
          "the following cycles keep the period-2 wave");
}

void testIndependentInstances() {
    Instance fast, slow;
    fast.setPeriod(2);
    slow.setPeriod(5);
    std::vector<uint64_t> fastWave, slowWave;
    for (int i = 0; i < 20; ++i) {
        fastWave.push_back(fast.cycle(0x3001));
        slowWave.push_back(slow.cycle(0x3002));
    }
    check(fastWave == expectedWave(2, 20),
          "the fast instance keeps its own period: " + describe(fastWave));
    check(slowWave == expectedWave(5, 20),
          "the slow instance keeps its own period: " + describe(slowWave));
    check(fastWave != slowWave, "two instances with different periods differ");
}

/* What the player types in the value window.  The test runs without the loader,
   so this is the literal fallback; with the loader present `tc.io_value` is the
   game's own evaluator and `1+7` means 8 (that half needs the game and is
   covered by the playtest for the window). */
void checkParse(const char* text, bool expectedOk, uint8_t expected,
                const std::string& what) {
    uint8_t value = 0;
    const bool ok = parseClockPeriod(text, &value);
    if (ok != expectedOk || (ok && value != expected)) {
        check(false, what + ": \"" + std::string(text ? text : "(null)") + "\" gave " +
                         (ok ? std::to_string(static_cast<unsigned>(value)) : "a refusal") +
                         ", expected " +
                         (expectedOk ? std::to_string(static_cast<unsigned>(expected))
                                     : std::string("a refusal")));
        return;
    }
    std::printf("ok    value text %-8s -> %s\n",
                text ? ("\"" + std::string(text) + "\"").c_str() : "(null)",
                ok ? std::to_string(static_cast<unsigned>(value)).c_str() : "refused");
}

void testValueText() {
    checkParse("1", true, 1, "the default value");
    checkParse("8", true, 8, "a plain number");
    checkParse("  12  ", true, 12, "surrounding spaces are ignored");
    checkParse("0x10", true, 16, "a hex literal");
    checkParse("0b101", true, 5, "a binary literal");
    checkParse("255", true, 255, "the widest value");
    /* Out of range is clamped, not refused: 0 would never flip at all. */
    checkParse("0", true, 1, "zero is clamped up to one");
    checkParse("300", true, 255, "past the widest value is clamped down");
    checkParse("", false, 0, "an empty field is refused");
    checkParse("   ", false, 0, "spaces alone are refused");
    checkParse("abc", false, 0, "letters are refused");
    checkParse("8x", false, 0, "trailing junk is refused");
    checkParse("1+7", false, 0, "an expression needs the game's own evaluator");
    checkParse(nullptr, false, 0, "a null field is refused");
}

/* ---- the face and the corner box ------------------------------------------
   renderClock talks to the host through the draw API, so a recorder standing in
   for that API is enough to check what it paints and where - including that the
   rectangle the click handler later hit-tests is the rectangle that was
   painted. */

struct Recorder {
    std::vector<float> x0, y0, x1, y1;
    int rects = 0, circles = 0, texts = 0;
    uint32_t colors = 0;
    uint32_t bitColor = 0;
    float bitX = 0.f, bitY = 0.f, bitRadius = 0.f;

    void line(float ax, float ay, float bx, float by, uint32_t color) {
        x0.push_back(ax); y0.push_back(ay);
        x1.push_back(bx); y1.push_back(by);
        colors |= color;
    }

    /* The face has two groups of strokes: the digits inside the corner box, and
       the type name at the other end of the same row. */
    bool inBox(size_t i) const {
        const auto inside = [](float x, float y) {
            return x >= kBadgeX0 - 0.01f && x <= kBadgeX1 + 0.01f &&
                   y >= kBadgeY0 - 0.01f && y <= kBadgeY1 + 0.01f;
        };
        return inside(x0[i], y0[i]) && inside(x1[i], y1[i]);
    }
    bool inName(size_t i) const { return x0[i] >= 0.2f && x1[i] >= 0.2f; }

    size_t digitStrokes() const {
        size_t count = 0;
        for (size_t i = 0; i < x0.size(); ++i) if (inBox(i)) ++count;
        return count;
    }
    size_t nameStrokes() const {
        size_t count = 0;
        for (size_t i = 0; i < x0.size(); ++i) if (inName(i)) ++count;
        return count;
    }
    /* Extremes of one group only. */
    float extreme(bool box, bool minimum, bool horizontal) const {
        float value = minimum ? 1.0e9f : -1.0e9f;
        for (size_t i = 0; i < x0.size(); ++i) {
            if (box ? !inBox(i) : !inName(i)) continue;
            const float a = horizontal ? x0[i] : y0[i];
            const float b = horizontal ? x1[i] : y1[i];
            if (minimum) value = std::min(value, std::min(a, b));
            else value = std::max(value, std::max(a, b));
        }
        return value;
    }

    float digitMinX() const { return extreme(true, true, true); }
    float digitMaxX() const { return extreme(true, false, true); }
    float digitMinY() const { return extreme(true, true, false); }
    float digitMaxY() const { return extreme(true, false, false); }
    float nameMinX() const { return extreme(false, true, true); }
    float nameMaxX() const { return extreme(false, false, true); }
    float nameMinY() const { return extreme(false, true, false); }
    float nameMaxY() const { return extreme(false, false, false); }
};

int recLine(void* context, float ax, float ay, float bx, float by, uint32_t color,
            float) {
    static_cast<Recorder*>(context)->line(ax, ay, bx, by, color);
    return 0;
}

int recRect(void* context, float, float, float, float, uint32_t, float, float) {
    ++static_cast<Recorder*>(context)->rects;
    return 0;
}
int recRectFilled(void* context, float, float, float, float, uint32_t, float) {
    ++static_cast<Recorder*>(context)->rects;
    return 0;
}
int recCircle(void* context, float, float, float, uint32_t, float) {
    ++static_cast<Recorder*>(context)->circles;
    return 0;
}
int recCircleFilled(void* context, float x, float y, float radius, uint32_t color) {
    Recorder* recorder = static_cast<Recorder*>(context);
    ++recorder->circles;
    recorder->bitColor = color;
    recorder->bitX = x;
    recorder->bitY = y;
    recorder->bitRadius = radius;
    return 0;
}
int recText(void* context, float, float, uint32_t, const char*) {
    ++static_cast<Recorder*>(context)->texts;
    return 0;
}

/* One painted face: what was drawn, and what the click handler was told. */
struct FacePaint {
    Recorder recorder;
    ClockBadge badge;
    bool badgeRecorded = false;
};

FacePaint paintFace(uint8_t period, uint64_t instanceId = 0x4001,
                    const uint8_t* configOverride = nullptr) {
    FacePaint paint;
    Recorder& recorder = paint.recorder;
    const TCComponentRenderDrawV1 draw{
        sizeof(TCComponentRenderDrawV1), TC_COMPONENT_RENDER_DRAW_VERSION_1,
        &recorder, &recLine, &recRect, &recRectFilled, &recCircle,
        &recCircleFilled, &recText};
    const uint8_t config[sizeof(ClockConfig)] = {period, 0};

    TCComponentRenderFrameV1 frame{};
    frame.size = sizeof(frame);
    frame.version = TC_COMPONENT_RENDER_FRAME_VERSION_1;
    frame.custom_id = kClockId;
    frame.instance_id = instanceId;
    /* Identity basis: local coordinates are screen coordinates, so the recorded
       numbers can be compared with the local face layout directly. */
    frame.axis_x_x = 1.f;
    frame.axis_y_y = 1.f;
    frame.clip_min_x = -100.f;
    frame.clip_min_y = -100.f;
    frame.clip_max_x = 100.f;
    frame.clip_max_y = 100.f;
    frame.config = configOverride ? configOverride : config;
    frame.config_size = sizeof(ClockConfig);
    frame.config_schema = kClockConfigSchema;
    frame.draw = &draw;

    renderClock(nullptr, &frame);
    paint.badgeRecorded = badgeOf(instanceId, &paint.badge);
    return paint;
}

void checkBadge(uint8_t period, size_t expectedSegments, const std::string& what) {
    const FacePaint paint = paintFace(period);
    if (paint.recorder.digitStrokes() != expectedSegments) {
        check(false, what + ": the box drew " +
                         std::to_string(paint.recorder.digitStrokes()) +
                         " segments, expected " + std::to_string(expectedSegments));
        return;
    }
    std::printf("ok    corner box %-4u -> %zu segment(s)\n",
                static_cast<unsigned>(period), expectedSegments);
}

void testCornerBox() {
    /* Segment counts of the seven-segment digits: 1 -> 2, 5 -> 5, 6 -> 6,
       8 -> 7, 10 -> 2 + 6, 100 -> 2 + 6 + 6, 255 -> 5 + 5 + 5. */
    checkBadge(1, 2, "a period of 1 draws a single '1'");
    checkBadge(5, 5, "a period of 5 draws the '5'");
    checkBadge(6, 6, "a period of 6 draws the '6'");
    checkBadge(8, 7, "a period of 8 draws the '8'");
    checkBadge(10, 8, "a period of 10 draws two digits");
    checkBadge(100, 14, "a period of 100 draws three digits");
    checkBadge(255, 15, "the widest period draws three digits");

    const FacePaint paint = paintFace(8);
    const Recorder& recorder = paint.recorder;
    check(recorder.texts == 0,
          "the box draws its number with strokes, not with the font");
    /* The body, its rim, the box and the box's rim.  Nothing else: the layout
       the stock constant uses is flat, with no inner panel. */
    check(recorder.rects == 4,
          "the face paints the body and the corner box, got " +
              std::to_string(recorder.rects));
    /* The body is as tall as the stock constant (2.93 cells) and as wide as the
       footprint can hold while the pin keeps its lane. */
    check(std::fabs((kBodyY1 - kBodyY0) - 2.93f) < 0.11f,
          "the body is as tall as the stock constant");
    check(kBodyX0 >= -kFootprintHalfWidth - 0.001f &&
              kBodyX1 <= kFootprintHalfWidth - 0.40f,
          "the body fits the footprint and leaves the pin lane free");
    check(std::fabs((kBodyX1 - kBodyX0) - 3.55f) < 0.11f,
          "the body takes the width the footprint allows");
    check(std::fabs((kBadgeX1 - kBadgeX0) - 1.00f) < 0.06f &&
              std::fabs((kBadgeY1 - kBadgeY0) - 0.62f) < 0.06f,
          "the number box is the stock constant's size");
    /* The pin is at (2,0) whatever the footprint is, so the body has to stop
       short of it and leave the same lane the stock part leaves - and the
       footprint itself has to stop short of the pin, which is what the player
       asked for: a box that wraps the pin swallows its drag area. */
    check(kBodyX1 <= 1.60f && kBodyX1 >= 1.40f,
          "the body ends 0.45 cells short of the output pin");
    check(kFootprintHalfWidth <= 2.0f,
          "the footprint does not reach the output pin at (2,0)");
    check(kBadgeX0 >= kBodyX0 && kBadgeY0 >= kBodyY0 && kBadgeX1 <= kBodyX1 &&
              kBadgeY1 <= kBodyY1,
          "the box sits inside the body");
    /* The type name sits on the top row, at the far end from the box, inside the
       body and clear of the output pin's approach. */
    check(recorder.nameStrokes() == 8,
          "the CLK name is eight strokes, got " +
              std::to_string(recorder.nameStrokes()));
    check(recorder.nameMinX() >= kBadgeX1 && recorder.nameMaxX() <= kNameX1 + 0.01f &&
              recorder.nameMinY() >= kNameY0 - 0.01f &&
              recorder.nameMaxY() <= kNameY1 + 0.01f,
          "the name stays in the body's top-right corner row");
    check(kNameX1 <= kBodyX1 - 0.20f,
          "the name keeps the stock part's margin from the body's right edge");
    /* The digits stay inside the box, and the box inside the body. */
    check(recorder.digitMinX() >= kBadgeX0 && recorder.digitMaxX() <= kBadgeX1 &&
              recorder.digitMinY() >= kBadgeY0 && recorder.digitMaxY() <= kBadgeY1,
          "the digits stay inside the corner box");
    /* The rectangle the click handler hit-tests must be the painted one: this is
       what makes "click the number" land on the number at any zoom or rotation. */
    check(paint.badgeRecorded, "the drawing code registered the box for the click handler");
    check(paint.badge.period == 8, "the registered box carries the painted period");
    check(std::fabs(paint.badge.min_x - kBadgeX0) < 0.001f &&
              std::fabs(paint.badge.min_y - kBadgeY0) < 0.001f &&
              std::fabs(paint.badge.max_x - kBadgeX1) < 0.001f &&
              std::fabs(paint.badge.max_y - kBadgeY1) < 0.001f,
          "the registered rectangle is the painted box");

    /* The render thread sees what a value window wrote before the board's own
       configuration copy catches up; that is what the mirror is for. */
    Instance instance;
    instance.setPeriod(20);
    (void)instance.run(0x4002, 1);
    {
        std::lock_guard<std::mutex> lock(g_clockMutex);
        const auto found = g_clocks.find(0x4002);
        check(found != g_clocks.end(), "the callback registers the instance's period");
    }
    const uint8_t stale[sizeof(ClockConfig)] = {1, 0};
    const FacePaint mirrored = paintFace(1, 0x4002, stale);
    /* "20" is a 2 (five segments) and a 0 (six). */
    check(mirrored.recorder.digitStrokes() == 11 && mirrored.badge.period == 20,
          "the box shows the mirror's period (20), not the stale configuration");

    /* A clock that has never run has no mirror entry yet: the box falls back to
       the configuration, so a freshly loaded board shows its saved value. */
    const FacePaint fresh = paintFace(16, 0x4003);
    /* "16" is a 1 (two segments) and a 6 (six). */
    check(fresh.recorder.digitStrokes() == 8 && fresh.badge.period == 16,
          "a box with no mirror entry shows the configuration's period");

    /* The value in the middle: one bit, drawn only once the callback has told
       the drawing code what it holds - never an invented level - and in the
       punch-tape colours the switch and button already use. */
    check(fresh.recorder.circles == 0 && !fresh.badge.levelKnown,
          "a clock that never ran draws no value in the middle");
    check(mirrored.recorder.circles == 1 && mirrored.badge.levelKnown,
          "a clock that has run draws its level in the middle");
    /* One cycle of a period-20 clock is still its first level, which is low. */
    check(mirrored.recorder.bitColor == rgba(239, 53, 83),
          "level 0 is the red bit");
    check(std::fabs(mirrored.recorder.bitX - kMarkX) < 0.01f &&
              std::fabs(mirrored.recorder.bitY) < 0.01f,
          "the value sits in the middle of the body, below the name row");
    check(std::fabs(mirrored.recorder.bitRadius - kMarkSize * 0.5f) < 0.01f,
          "the hand-drawn fallback is the size the sprite would be");
    /* The sprite is the game's own asset/io_state/io_state.png: column 0 is the
       red low state, column 1 the green high state.  The mark itself needs the
       real engine (the unit test has no texture), so the unit test pins the
       geometry and this mapping, and the playtest covers the rest. */
    check(spriteColumn(0) == kSpriteColumnLow && spriteColumn(1) == kSpriteColumnHigh,
          "level 0 picks the red column and level 1 the green one");

    Instance high;
    high.setPeriod(1);
    (void)high.run(0x4005, 1);         /* one cycle of a period-1 clock: high */
    const FacePaint highFace = paintFace(1, 0x4005);
    check(highFace.recorder.circles == 1 &&
              highFace.recorder.bitColor == rgba(34, 177, 78),
          "level 1 is the green bit");
}

void testWithoutServices() {
    /* The plugin has to survive a loader whose component services are missing:
       the click path reports the failure instead of crashing, and the
       registration-free unit test simply has no tables loaded. */
    check(!storeClockPeriod(0x5001, 4),
          "storing a period without the instance services reports a failure");
    check(g_instancesApi.enumerate == nullptr && g_storageApi.write_config == nullptr,
          "the services are indeed unloaded in this test");
}

}  // namespace

int main() {
    testWaves();
    testResetAndRefresh();
    testIndependentInstances();
    testValueText();
    testCornerBox();
    testWithoutServices();
    if (g_failures) {
        std::printf("clock period: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("clock period: all checks passed\n");
    return 0;
}
