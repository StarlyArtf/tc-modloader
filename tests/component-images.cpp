/* tc.component.render V7's bookkeeping: the names, handles and lifetime rules
   behind the images a Mod's own draw callback paints.

   The GL objects belong to ui_texture::Manager (and are covered by its own
   limits), so what is pinned here is the part the loader adds on top:

   * the key rules a Mod has to follow;
   * a handle that never changes while the key stays loaded, and a revision that
     goes up when the same key is loaded again with new pixels;
   * the previous texture is retired on replace and on release, so a Mod never
     has to free anything;
   * the per-Mod quota (images and decoded bytes) and the promise that a refused
     call changes nothing;
   * a handle that was never loaded - or one from another Mod's table - is not
     found, which is how "resources are isolated per Mod" is enforced.

   Built and run by build.ps1 ("Component images test"). */

#include "../src/component_images.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool value, const char* what) {
    if (value) return;
    std::printf("FAIL %s\n", what);
    std::fflush(stdout);
    ++failures;
}

using tc::component_images::Image;
using tc::component_images::Table;

constexpr uint64_t kMiB = 1024ull * 1024ull;

void test_keys() {
    expect(tc::component_images::validKey("sprite"), "a plain key is accepted");
    expect(tc::component_images::validKey("fp32.icons_1-x"), "dots, dashes and digits are accepted");
    expect(!tc::component_images::validKey(""), "an empty key is refused");
    expect(!tc::component_images::validKey("Sprite"), "an uppercase key is refused");
    expect(!tc::component_images::validKey("sp rite"), "a space is refused");
    expect(!tc::component_images::validKey(std::string(65, 'a')),
           "a key longer than 64 bytes is refused");
}

void test_load_replace_release() {
    Table table;
    std::vector<uint64_t> retired;
    const Table::Retire retire = [&](uint64_t texture) { retired.push_back(texture); };
    uint64_t next = 1;
    Image out{};

    expect(table.load("sprite", 100, 7, 64, 32, 64 * 32 * 4, &next, retire, &out) ==
               TC_COMPONENT_RENDER_OK,
           "the first load succeeds");
    const uint64_t handle = out.image;
    expect(handle == 1, "handles come from the caller's counter");
    expect(out.revision == 1, "a fresh image starts at revision 1");
    expect(out.width == 64 && out.height == 32, "the size is reported back");
    expect(table.count() == 1, "the image is in the table");
    expect(table.bytes() == 64ull * 32 * 4, "the byte total is kept");
    const auto* found = table.find(handle);
    expect(found && found->texture == 100 && found->renderer == 7,
           "the draw call finds the texture and the renderer id");
    expect(table.find(0) == nullptr, "handle 0 is never an image");
    expect(table.find(4242) == nullptr, "an unknown handle is not found");

    /* Replacing keeps the handle a Mod already draws with, retires the old
       texture and moves the revision. */
    expect(table.load("sprite", 200, 8, 16, 16, 16 * 16 * 4, &next, retire, &out) ==
               TC_COMPONENT_RENDER_OK,
           "replacing the same key succeeds");
    expect(out.image == handle, "the handle a Mod holds stays valid");
    expect(out.revision == 2, "the revision moves on a replace");
    expect(out.width == 16 && out.height == 16, "the new size is reported");
    expect(retired.size() == 1 && retired[0] == 100, "the replaced texture is retired");
    expect(table.find(handle) && table.find(handle)->texture == 200,
           "the draw call now paints the new texture");
    expect(table.count() == 1, "a replace does not add an entry");
    expect(table.bytes() == 16ull * 16 * 4, "the byte total follows the new image");
    expect(next == 2, "a replace does not burn a handle");

    /* A second key gets its own handle. */
    expect(table.load("icon", 300, 9, 8, 8, 8 * 8 * 4, &next, retire, &out) ==
               TC_COMPONENT_RENDER_OK,
           "a second key is accepted");
    expect(out.image == 2 && out.image != handle, "each image has its own handle");
    expect(table.count() == 2, "both images are in the table");

    expect(table.clear("sprite", retire) == TC_COMPONENT_RENDER_OK, "release drops one key");
    expect(retired.size() == 2 && retired[1] == 200, "releasing retires that texture");
    expect(table.find(handle) == nullptr, "the released handle is gone");
    expect(table.count() == 1, "the other key survives");
    expect(table.clear("sprite", retire) == TC_COMPONENT_RENDER_ERR_IMAGE,
           "releasing an unknown key says so");

    expect(table.clear("", retire) == TC_COMPONENT_RENDER_OK, "an empty key releases everything");
    expect(table.count() == 0 && table.bytes() == 0, "the table is empty again");
    expect(retired.size() == 3 && retired[2] == 300, "the last texture is retired too");

    /* forget() is what an unloaded Mod uses: the texture manager retires its
       whole set at once, so the table must not ask for a second release. */
    uint64_t nextAgain = 1;
    expect(table.load("sprite", 400, 1, 4, 4, 4 * 4 * 4, &nextAgain, retire, &out) ==
               TC_COMPONENT_RENDER_OK,
           "the table can be filled again");
    const size_t before = retired.size();
    table.forget();
    expect(table.count() == 0, "forget empties the table");
    expect(retired.size() == before, "forget does not retire anything by itself");
}

void test_quota() {
    Table table;
    const Table::Retire none = [](uint64_t) {};
    uint64_t next = 1;
    Image out{};

    for (uint32_t index = 0; index < TC_COMPONENT_RENDER_MAX_IMAGES; ++index) {
        const std::string key = "img" + std::to_string(index);
        expect(table.load(key, 1000 + index, index, 2, 2, 2 * 2 * 4, &next, none, &out) ==
                   TC_COMPONENT_RENDER_OK,
               "an image inside the count quota is accepted");
    }
    expect(table.count() == TC_COMPONENT_RENDER_MAX_IMAGES, "the table is full");
    expect(table.load("one-too-many", 999, 1, 2, 2, 16, &next, none, &out) ==
               TC_COMPONENT_RENDER_ERR_IMAGE,
           "one more image is refused");
    expect(table.count() == TC_COMPONENT_RENDER_MAX_IMAGES, "a refused load adds nothing");

    /* Bytes: a single image beyond the cap, and a replace that would push the
       total over it. */
    Table bytes;
    uint64_t nextBytes = 1;
    expect(bytes.load("huge", 1, 1, 4096, 4096, 4096ull * 4096 * 4, &nextBytes, none, &out) ==
               TC_COMPONENT_RENDER_ERR_IMAGE,
           "an image larger than the byte cap is refused");
    expect(bytes.load("half", 2, 1, 1024, 1024, 4 * kMiB, &nextBytes, none, &out) ==
               TC_COMPONENT_RENDER_OK,
           "an image under the cap is accepted");
    expect(bytes.load("third", 3, 1, 1024, 1024, 4 * kMiB, &nextBytes, none, &out) ==
               TC_COMPONENT_RENDER_OK,
           "a second one is accepted");
    expect(bytes.load("last", 4, 1, 1024, 1024, 4 * kMiB, &nextBytes, none, &out) ==
               TC_COMPONENT_RENDER_OK,
           "a third one is accepted");
    expect(bytes.load("overflow", 5, 1, 1024, 1024, 8 * kMiB, &nextBytes, none, &out) ==
               TC_COMPONENT_RENDER_ERR_IMAGE,
           "an image that would exceed the Mod's byte budget is refused");
    expect(bytes.count() == 3, "a refused byte quota adds nothing");
    expect(bytes.load("overflow", 6, 1, 1024, 1024, 4 * kMiB, &nextBytes, none, &out) ==
               TC_COMPONENT_RENDER_OK,
           "the byte left over is still usable");
    expect(bytes.bytes() <= TC_COMPONENT_RENDER_MAX_IMAGE_BYTES,
           "the byte total stays inside the cap");
}

void test_isolation_between_mods() {
    Table first;
    Table second;
    const Table::Retire none = [](uint64_t) {};
    uint64_t next = 1;
    Image out{};

    expect(first.load("sprite", 100, 1, 4, 4, 64, &next, none, &out) ==
               TC_COMPONENT_RENDER_OK,
           "the first Mod loads an image");
    const uint64_t handle = out.image;
    expect(second.load("sprite", 200, 2, 4, 4, 64, &next, none, &out) ==
               TC_COMPONENT_RENDER_OK,
           "the second Mod loads its own image under the same key");
    expect(out.image != handle, "handles are unique across Mods");
    expect(second.find(handle) == nullptr,
           "the first Mod's handle is unknown in the second Mod's table");
    expect(first.find(handle) != nullptr, "the owner still finds it");
}

void test_isolation_between_instances() {
    Table table;
    const Table::Retire none=[](uint64_t){};
    uint64_t next=1;Image out{};
    TCComponentInstanceHandle first{};first.custom_id=10;first.instance_id=77;first.generation=1;
    TCComponentInstanceHandle second=first;second.instance_id=78;
    expect(table.loadScoped(&first,"screen",501,51,8,8,256,&next,none,&out)==
               TC_COMPONENT_RENDER_OK,"an instance-scoped image loads");
    const uint64_t handle=out.image;
    expect(table.find(handle,&first)!=nullptr,"the owning instance resolves its image");
    expect(table.find(handle,&second)==nullptr,"another instance cannot resolve the handle");
    TCComponentInstanceHandle stale=first;++stale.generation;
    expect(table.find(handle,&stale)==nullptr,"a recycled instance generation cannot resolve it");
    std::vector<uint64_t> retired;
    table.prune([](uint64_t,uint64_t instance,uint64_t generation){
        return instance==77&&generation==2;
    },[&](uint64_t texture){retired.push_back(texture);});
    expect(table.find(handle,&first)==nullptr&&retired.size()==1&&retired[0]==501,
           "stale instance resources are retired by pruning");
}

}  // namespace

int main() {
    test_keys();
    test_load_replace_release();
    test_quota();
    test_isolation_between_mods();
    test_isolation_between_instances();
    if (failures) {
        std::printf("FAIL component images (%d)\n", failures);
        return 1;
    }
    std::printf("PASS component images: keys, handle stability, revisions, retirement, "
                "quota, per-Mod and per-instance isolation are pinned\n");
    return 0;
}
