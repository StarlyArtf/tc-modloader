#include "../src/preview_label_identity.hpp"

#include <cassert>
#include <iostream>

using tc::preview_labels::samePin;

int main() {
    /* A left-edge input label grows to the left.  Its visual centre moves by
       more than a hundred pixels, but the bar endpoint and direction do not. */
    assert(samePin(true, 320.f, 180.f, -1.f, 0.f, 300.f, 180.f,
                   true, 320.f, 180.f, -1.f, 0.f, 185.f, 180.f));

    /* The symmetric output case: shortening the text moves its centre too. */
    assert(samePin(true, 760.f, 228.f, 1.f, 0.f, 900.f, 228.f,
                   true, 760.f, 228.f, 1.f, 0.f, 782.f, 228.f));

    /* Nearby pins and opposite-facing pins must not inherit each other's row. */
    assert(!samePin(true, 320.f, 180.f, -1.f, 0.f, 300.f, 180.f,
                    true, 320.f, 204.f, -1.f, 0.f, 185.f, 204.f));
    assert(!samePin(true, 320.f, 180.f, -1.f, 0.f, 300.f, 180.f,
                    true, 320.f, 180.f, 1.f, 0.f, 340.f, 180.f));

    /* The very first frame has no bars and intentionally falls back to the old
       centre match; once bars exist, label width no longer matters. */
    assert(samePin(false, 0.f, 0.f, 0.f, 0.f, 410.f, 96.f,
                   false, 0.f, 0.f, 0.f, 0.f, 411.f, 96.f));
    assert(!samePin(false, 0.f, 0.f, 0.f, 0.f, 410.f, 96.f,
                    false, 0.f, 0.f, 0.f, 0.f, 430.f, 96.f));

    std::cout << "PASS preview label identity: input/output renames keep their pin rows\n";
}
