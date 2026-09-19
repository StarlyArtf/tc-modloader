#include "../sdk/tc_ui.h"
#include <cassert>
#include <type_traits>
#include <vector>
#include <iostream>
static std::vector<std::string> stack;
static int pops;
static void push(const char* id) { stack.emplace_back(id); }
static void pop() { assert(!stack.empty()); stack.pop_back(); ++pops; }
int main() {
    using tc::ui::Id;
    static_assert(!std::is_copy_constructible_v<Id> && !std::is_move_constructible_v<Id>);
    { Id unloaded("mod"); }
    tc::ui::table().pushId=push;
    { Id partial("mod"); }
    assert(stack.empty());
    tc::ui::table().popId=pop;
    { Id empty(nullptr); }
    assert(pops==0);
    try {
        Id mod("example.one");
        { Id page("settings"); assert(stack==std::vector<std::string>({"example.one","settings"})); }
        assert(stack.size()==1 && pops==1);
        throw 1;
    } catch(int) {}
    assert(stack.empty() && pops==2);
    std::cout<<"PASS UI ID scope: unavailable/partial APIs, nesting, exception cleanup\n";
}
