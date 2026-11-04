// The scope fixture uses the real example and resolves its model through the
// Board handle, so drawer layout cannot stall the level's automated test.
#define tc_mod_load scope_byte_adder_base_load
#include "../examples/byte-adder/plugin.cpp"
#undef tc_mod_load
namespace {
void (*scopeAdderBaseFrame)(void*,const TCFrame*)=nullptr;
void scopeAdderFrame(void* user,const TCFrame* value) {
    if(!value)return;
    if(!autotestModel) {
        TCGameHandle board{};board.size=sizeof(board);const void* model=nullptr;
        if(tc::currentGameHandle(host,TC_GAME_OBJECT_BOARD,&board)==TC_HANDLE_OK &&
           tc::resolveGameHandle(host,&board,&model)==TC_HANDLE_OK && model) {
            autotestModel=const_cast<void*>(model);
            log("byte-adder: scope fixture resolved Board model");
        }
    }
    if(scopeAdderBaseFrame)scopeAdderBaseFrame(user,value);
    if(autotestModel)autotestElapsed=0; // stop synthetic menu presses after entry
}
}
extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h,TCPlugin* plugin) {
    const int result=scope_byte_adder_base_load(h,plugin);if(result)return result;
    scopeAdderBaseFrame=plugin->on_frame;plugin->on_frame=&scopeAdderFrame;return 0;
}
