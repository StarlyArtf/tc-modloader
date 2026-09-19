#define main existing_fixture_main
#include "and-component-fixture.cpp"
#undef main
/* The custom component this board places.  Defaults to the declarative byte
   adder; tests/pin-label-playtest.ps1 overrides it with the long-pin fixture's
   id so the same board shows a component with pins on the top and bottom. */
#ifndef TC_BOARD_CUSTOM_ID
#define TC_BOARD_CUSTOM_ID 0x414444385F303031ULL
#endif
#ifndef TC_BOARD_OUTPUT
#define TC_BOARD_OUTPUT "build/declarative-8x8-board.data"
#endif
int main() {
    Writer w;
    w.i64(0x6f13c29bb2e19440LL);w.u32(0);w.i64(0);w.i64(0);w.u8(1);w.i64(10000);
    w.sequence_i64({});w.string("");w.u8(0);w.u16(0);w.sequence_u8({});w.string("");
    for(int i=0;i<512;++i)w.u8(0);
    w.i64(1);addV13CustomInstance(w,-5,0,0x2222222222222222ULL,TC_BOARD_CUSTOM_ID);
    w.i64(10);
    const int sources[]={-12,-4,4,-12,-4,-12,-4,4};
    for(int i=0;i<8;++i) {
        w.u8(0);w.string("");w.i16(-17);w.i16(sources[i]);
        // Separate horizontal lanes; wire intersections are not junctions.
        w.u16(2+i);const int delta=(i-3)-sources[i];
        if(delta)w.u16((delta>0?0x4000:0xc000)|std::abs(delta));
        w.u16(8-i);w.u16(0);
    }
    addWire(w,-3,6,{0xc00a,0x0015,0});
    addWire(w,-3,7,{0x0015,0xc003,0});
    std::vector<uint8_t> encoded{13};writeLiteralSnappy(w.bytes,encoded);
    std::ofstream file(TC_BOARD_OUTPUT,std::ios::binary);
    file.write(reinterpret_cast<const char*>(encoded.data()),encoded.size());
    return file?0:1;
}
