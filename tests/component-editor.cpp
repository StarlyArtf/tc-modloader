#include "../src/component_editor.hpp"
#include "../sdk/tc_service_api.h"
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

/* The ABI of the declaration table is pinned here as well as in the snapshot:
   a Mod built against V1 hands the loader an array of these, so a field moving
   would silently re-interpret every Mod's table. */
static_assert(offsetof(TCComponentFieldV1,field_id)==0);
static_assert(offsetof(TCComponentFieldV1,label)==8);
static_assert(offsetof(TCComponentFieldV1,tooltip)==16);
static_assert(offsetof(TCComponentFieldV1,kind)==24);
static_assert(offsetof(TCComponentFieldV1,flags)==28);
static_assert(offsetof(TCComponentFieldV1,offset)==32);
static_assert(offsetof(TCComponentFieldV1,bytes)==36);
static_assert(offsetof(TCComponentFieldV1,bits)==40);
static_assert(offsetof(TCComponentFieldV1,choices)==44);
static_assert(offsetof(TCComponentFieldV1,choice_labels)==48);
static_assert(offsetof(TCComponentFieldV1,min_value)==56);
static_assert(offsetof(TCComponentFieldV1,max_value)==64);
static_assert(offsetof(TCComponentFieldV1,min_float)==72);
static_assert(offsetof(TCComponentFieldV1,max_float)==80);
static_assert(sizeof(TCComponentFieldV1)==88);
static_assert(offsetof(TCComponentEditorApiV1,set_fields)==16);
static_assert(offsetof(TCComponentEditorApiV1,clear_fields)==24);
static_assert(sizeof(TCComponentEditorApiV1)==32);
/* V2 repeats the V1 table: the version is what tells a Mod that this loader
   understands the layout flags, and the table itself does not change shape. */
static_assert(offsetof(TCComponentEditorApiV2,set_fields)==16);
static_assert(offsetof(TCComponentEditorApiV2,clear_fields)==24);
static_assert(sizeof(TCComponentEditorApiV2)==32);
static_assert(offsetof(TCComponentEditorApiV3,set_fields)==16);
static_assert(offsetof(TCComponentEditorApiV3,set_fields_v2)==32);
static_assert(sizeof(TCComponentEditorApiV3)==64);
static_assert(offsetof(TCComponentFieldV2,field)==8);
/* V4 keeps every V3 member where it was and appends the table-level entry point,
   so a V3 table handed to a V4 loader still means exactly what it meant. */
static_assert(offsetof(TCComponentEditorApiV4,set_fields)==16);
static_assert(offsetof(TCComponentEditorApiV4,set_fields_v2)==32);
static_assert(offsetof(TCComponentEditorApiV4,begin_edit)==40);
static_assert(offsetof(TCComponentEditorApiV4,set_fields_v3)==64);
static_assert(sizeof(TCComponentEditorApiV4)==72);
static_assert(TC_COMPONENT_FIELD_COLOR_PALETTE==(1u<<8));
static_assert(TC_COMPONENT_FIELD_COLOR_HEX==(1u<<9));
static_assert(TC_COMPONENT_FIELD_VALIDATION_ICON==(1u<<10));
static_assert(TC_COMPONENT_EDITOR_TABLE_CONFIRM==1u);

using namespace tc::component_editor;

namespace {

Field declared(uint32_t kind,uint32_t offset,uint32_t bytes,uint32_t bits=0){
    TCComponentFieldV1 value{};
    value.field_id="f";
    value.label="F";
    value.kind=kind;
    value.offset=offset;
    value.bytes=bytes;
    value.bits=bits;
    return fromDeclared(value);
}

Field enumField(uint32_t bytes,const std::vector<std::string>& choices){
    TCComponentFieldV1 value{};
    value.field_id="mode";
    value.label="Mode";
    value.kind=TC_COMPONENT_FIELD_ENUM;
    value.offset=0;
    value.bytes=bytes;
    static std::vector<const char*> labels;
    labels.clear();
    for(const auto& choice:choices)labels.push_back(choice.c_str());
    value.choices=static_cast<uint32_t>(choices.size());
    value.choice_labels=labels.data();
    return fromDeclared(value);
}

std::string why;

}  // namespace

int main(){
    /* ---- validation -------------------------------------------------- */
    Field field=declared(TC_COMPONENT_FIELD_BOOL,0,1);
    assert(validate(field,4,why)&&field.bytes==1);
    field=declared(TC_COMPONENT_FIELD_BOOL,0,2);
    assert(!validate(field,8,why)&&why.find("not one byte")!=std::string::npos);
    field=declared(TC_COMPONENT_FIELD_INT,4,4);
    assert(validate(field,8,why)&&!field.hasIntRange);
    field=declared(TC_COMPONENT_FIELD_INT,5,4);
    assert(!validate(field,8,why)&&why.find("past the configuration")!=std::string::npos);
    field=declared(TC_COMPONENT_FIELD_INT,0,3);
    assert(!validate(field,8,why));
    field=declared(TC_COMPONENT_FIELD_UINT,0,1,8);
    assert(validate(field,1,why)&&field.maxUnsigned==255);
    field=declared(TC_COMPONENT_FIELD_UINT,0,2,12);
    assert(validate(field,2,why)&&field.maxUnsigned==4095);
    /* A 32-bit unsigned value cannot be typed in the V1 widget, so the field is
       refused rather than drawn as something that cannot hold it. */
    field=declared(TC_COMPONENT_FIELD_UINT,0,4,0);
    assert(!validate(field,4,why)&&why.find("bits")!=std::string::npos);
    field=declared(TC_COMPONENT_FIELD_TEXT,0,1);
    assert(!validate(field,1,why)&&why.find("text bytes")!=std::string::npos);
    field=declared(TC_COMPONENT_FIELD_TEXT,0,65);
    assert(!validate(field,65,why));
    field=declared(TC_COMPONENT_FIELD_FLOAT,0,8);
    assert(!validate(field,8,why));
    field=declared(TC_COMPONENT_FIELD_FLOAT,0,4);
    assert(validate(field,4,why));
    field=declared(99,0,1);
    assert(!validate(field,1,why));
    field=enumField(1,{"a","b","c"});
    assert(validate(field,1,why)&&field.choices.size()==3);
    field=enumField(1,{});
    assert(!validate(field,1,why)&&why.find("no choices")!=std::string::npos);
    field=enumField(1,{"a","b"});
    field.bytes=0;   /* a zero-width enum is refused before the choices are read */
    assert(!validate(field,1,why));

    /* ---- little-endian round trips ----------------------------------- */
    std::vector<uint8_t> config(16,0xEE);
    field=declared(TC_COMPONENT_FIELD_UINT,1,2,12);
    assert(validate(field,config.size(),why));
    Value value;value.integer=0x0ABC;
    assert(apply(field,config.data(),config.size(),value,nullptr));
    assert(config[0]==0xEE&&config[1]==0xBC&&config[2]==0x0A&&config[3]==0xEE);
    Value back{};
    assert(read(field,config.data(),config.size(),back)&&back.integer==0x0ABC);
    /* Writing one field leaves its neighbours alone. */
    assert(config[0]==0xEE&&config[3]==0xEE);

    field=declared(TC_COMPONENT_FIELD_INT,0,2);
    assert(validate(field,config.size(),why));
    value.integer=-2;
    assert(apply(field,config.data(),config.size(),value,nullptr));
    assert(config[0]==0xFE&&config[1]==0xFF);
    assert(read(field,config.data(),config.size(),back)&&back.integer==-2);

    /* A declared range narrows the clamp; the note names the field. */
    field=declared(TC_COMPONENT_FIELD_INT,0,1);
    field.minValue=-4;field.maxValue=4;
    assert(validate(field,config.size(),why)&&field.hasIntRange);
    value.integer=99;
    std::string note;
    assert(apply(field,config.data(),config.size(),value,&note));
    assert(!note.empty()&&note.find("\"f\"")!=std::string::npos);
    assert(read(field,config.data(),config.size(),back)&&back.integer==4);

    field=declared(TC_COMPONENT_FIELD_UINT,0,1,5);
    assert(validate(field,config.size(),why));
    value.integer=200;
    note.clear();
    assert(apply(field,config.data(),config.size(),value,&note));
    assert(read(field,config.data(),config.size(),back)&&back.integer==31);

    /* ---- bool --------------------------------------------------------- */
    field=declared(TC_COMPONENT_FIELD_BOOL,2,1);
    assert(validate(field,config.size(),why));
    value.boolean=true;
    assert(apply(field,config.data(),config.size(),value,nullptr));
    assert(config[2]==1);
    value.boolean=false;
    assert(apply(field,config.data(),config.size(),value,nullptr));
    assert(config[2]==0);
    config[2]=7;   /* anything non-zero reads as on, and writes back as 1 */
    assert(read(field,config.data(),config.size(),back)&&back.boolean);

    /* ---- enum --------------------------------------------------------- */
    field=enumField(1,{"RNE","RTZ","RDN","RUP","RNA"});
    assert(validate(field,config.size(),why));
    value.integer=2;
    assert(apply(field,config.data(),config.size(),value,nullptr));
    assert(config[0]==2);
    assert(read(field,config.data(),config.size(),back)&&back.integer==2&&back.text=="RDN");
    assert(display(field,back)=="RDN");
    assert(nextChoice(field,4)==0&&nextChoice(field,1)==2&&nextChoice(field,-1)==0);
    value.integer=99;
    note.clear();
    assert(apply(field,config.data(),config.size(),value,&note));
    assert(read(field,config.data(),config.size(),back)&&back.integer==4);

    /* ---- text --------------------------------------------------------- */
    field=declared(TC_COMPONENT_FIELD_TEXT,3,8);
    assert(validate(field,config.size(),why));
    value.text="hello";
    assert(apply(field,config.data(),config.size(),value,nullptr));
    assert(std::memcmp(config.data()+3,"hello\0\0\0",8)==0);
    assert(read(field,config.data(),config.size(),back)&&back.text=="hello");
    /* A full buffer is truncated to leave room for the terminator, and the
       bytes past it are cleared so the field cannot read stale text. */
    value.text="0123456789";
    note.clear();
    assert(apply(field,config.data(),config.size(),value,&note));
    assert(!note.empty()&&back.text!="0123456789");
    assert(read(field,config.data(),config.size(),back)&&back.text=="0123456");
    assert(config[3+7]==0);
    value.text="";
    assert(apply(field,config.data(),config.size(),value,nullptr));
    assert(read(field,config.data(),config.size(),back)&&back.text.empty());

    /* ---- float -------------------------------------------------------- */
    field=declared(TC_COMPONENT_FIELD_FLOAT,0,4);
    assert(validate(field,config.size(),why));
    value.real=1.5;
    assert(apply(field,config.data(),config.size(),value,nullptr));
    assert(config[0]==0x00&&config[1]==0x00&&config[2]==0xC0&&config[3]==0x3F);
    assert(read(field,config.data(),config.size(),back)&&back.real==1.5);
    field.minFloat=0.0;field.maxFloat=1.0;
    assert(validate(field,config.size(),why)&&field.hasFloatRange);
    value.real=7.5;
    note.clear();
    assert(apply(field,config.data(),config.size(),value,&note));
    assert(read(field,config.data(),config.size(),back)&&back.real==1.0);

    /* ---- display and refusals ---------------------------------------- */
    field=declared(TC_COMPONENT_FIELD_UINT,0,2,16);
    assert(validate(field,config.size(),why));
    assert(field.maxUnsigned==65535);
    value.integer=0xBEEF;
    assert(display(field,value)=="48879");
    field.hex=true;
    assert(display(field,value)=="0xbeef");
    /* Reading through a configuration that is too short is refused instead of
       reaching past it. */
    assert(!read(field,config.data(),1,back));
    Value untouched=back;
    assert(!apply(declared(TC_COMPONENT_FIELD_UINT,8,2),config.data(),4,value,nullptr));
    assert(back.integer==untouched.integer);
    /* A float field carries its bits through unchanged, including the NaN
       pattern a Mod may use as "unset". */
    field=declared(TC_COMPONENT_FIELD_FLOAT,0,4);
    assert(validate(field,config.size(),why));
    value.real=NAN;
    assert(apply(field,config.data(),config.size(),value,nullptr));
    assert(read(field,config.data(),config.size(),back)&&std::isnan(back.real));

    /* ---- V2: layout flags, radio, and the hexadecimal widget ---------- */
    /* The hexadecimal widget is what lifts the unsigned ceiling from the signed
       decimal box's 31 bits to 64. */
    field=declared(TC_COMPONENT_FIELD_UINT,0,4);
    field.hex=true;
    field.bits=32;
    assert(validate(field,config.size(),why)&&field.maxUnsigned==0xffffffffULL);
    field=declared(TC_COMPONENT_FIELD_UINT,0,8);
    field.hex=true;
    assert(validate(field,config.size(),why)&&field.maxUnsigned==~uint64_t(0));
    field=declared(TC_COMPONENT_FIELD_UINT,0,4);
    field.hex=true;
    field.bits=33;   /* wider than the bytes it declares */
    assert(!validate(field,config.size(),why));
    field=declared(TC_COMPONENT_FIELD_UINT,0,8);
    field.hex=true;
    field.bytes=8;field.bits=0;
    assert(validate(field,config.size(),why));
    /* Without the flag the decimal widget keeps its own ceiling. */
    field=declared(TC_COMPONENT_FIELD_UINT,0,4);
    assert(!validate(field,config.size(),why));

    /* The layout flags are accepted, and an unknown bit is refused instead of
       being ignored (a typo would otherwise change the layout silently). */
    field=declared(TC_COMPONENT_FIELD_UINT,0,2);
    field.fullRow=true;
    field.separatorBefore=true;
    assert(validate(field,config.size(),why));
    field=declared(TC_COMPONENT_FIELD_UINT,0,2);
    field.flags=1u<<15;
    assert(!validate(field,config.size(),why)&&why.find("unknown flags")!=std::string::npos);
    /* Radio buttons are an enum's shape, not a flag any field may carry. */
    field=declared(TC_COMPONENT_FIELD_BOOL,0,1);
    field.radio=true;
    assert(!validate(field,config.size(),why)&&why.find("radio")!=std::string::npos);

    /* V3 adds a real combo shape, RGBA8, and a custom interaction canvas. */
    field=enumField(1,{"a","b"});field.dropdown=true;
    assert(validate(field,config.size(),why));
    field.radio=true;
    assert(!validate(field,config.size(),why)&&why.find("both radio and dropdown")!=std::string::npos);
    field=declared(TC_COMPONENT_FIELD_COLOR_RGBA8,0,4);
    assert(validate(field,config.size(),why));
    value.integer=static_cast<int64_t>(UINT32_C(0x80402010));
    assert(apply(field,config.data(),config.size(),value,nullptr));
    assert(read(field,config.data(),config.size(),back)&&
           static_cast<uint32_t>(back.integer)==UINT32_C(0x80402010));
    /* The packed word is 0xAABBGGRR, so the literal reads the other way round:
       0x80402010 is R=0x10, G=0x20, B=0x40, A=0x80. */
    assert(display(field,back)=="#10204080");
    assert(formatColorLiteral(UINT32_C(0x80402010))=="#10204080");
    assert(formatColorLiteral(UINT32_C(0xff4020ff))=="#ff2040");
    field=declared(TC_COMPONENT_FIELD_COLOR_RGBA8,0,3);
    assert(!validate(field,config.size(),why)&&why.find("four bytes")!=std::string::npos);
    field=declared(TC_COMPONENT_FIELD_CUSTOM,0,0);field.resizeHandle=true;
    field.customHeight=48.f;
    assert(validate(field,config.size(),why));
    Field afterCustom=declared(TC_COMPONENT_FIELD_BOOL,0,1);afterCustom.inlineWithPrevious=true;
    std::vector<Field> customTable{field,afterCustom};
    assert(!validateTable(customTable,config.size(),why)&&why.find("whole line")!=std::string::npos);

    /* V4: a colour row can be a swatch palette, an exact hexadecimal box, or
       both, and a validation message can be a marker instead of a line. */
    {
        static const char* const kSwatches[]={"#ff4040","#ffa030","#0a0"};
        TCComponentFieldV1 declaredPalette{};
        declaredPalette.field_id="palette";declaredPalette.label="Palette";
        declaredPalette.kind=TC_COMPONENT_FIELD_COLOR_RGBA8;
        declaredPalette.flags=TC_COMPONENT_FIELD_COLOR_PALETTE|TC_COMPONENT_FIELD_COLOR_HEX;
        declaredPalette.offset=0;declaredPalette.bytes=4;
        declaredPalette.choices=3;declaredPalette.choice_labels=kSwatches;
        Field palette=fromDeclared(declaredPalette);
        assert(validate(palette,config.size(),why));
        assert(palette.swatches.size()==3);
        assert(palette.swatches[0]==UINT32_C(0xff4040ff));
        assert(palette.swatches[1]==UINT32_C(0xff30a0ff));
        /* "#0a0" is the three-digit shorthand, expanded the way CSS does. */
        assert(palette.swatches[2]==UINT32_C(0xff00aa00));
        assert(palette.colorPalette&&palette.colorHex);
        Value swatchValue{};swatchValue.integer=static_cast<int64_t>(UINT32_C(0xff00aa00));
        assert(display(palette,swatchValue)=="#00aa00");

        /* A palette with no swatches and one whose literal is not a colour are
           both refused: the table is a contract, not a guess. */
        declaredPalette.choices=0;
        palette=fromDeclared(declaredPalette);
        assert(!validate(palette,config.size(),why)&&why.find("no swatches")!=std::string::npos);
        static const char* const kBroken[]={"#ff4040","nope","#0a0"};
        declaredPalette.choices=3;declaredPalette.choice_labels=kBroken;
        palette=fromDeclared(declaredPalette);
        assert(!validate(palette,config.size(),why)&&why.find("not a colour")!=std::string::npos);

        /* The two flags belong to the colour kind; anything else is refused. */
        Field wrong=declared(TC_COMPONENT_FIELD_UINT,0,1,8);
        wrong.colorPalette=true;wrong.flags=TC_COMPONENT_FIELD_COLOR_PALETTE;
        assert(!validate(wrong,config.size(),why)&&why.find("not a colour")!=std::string::npos);
        wrong=declared(TC_COMPONENT_FIELD_ENUM,0,1);
        wrong.colorHex=true;wrong.flags=TC_COMPONENT_FIELD_COLOR_HEX;
        assert(!validate(wrong,config.size(),why)&&why.find("not a colour")!=std::string::npos);

        /* And the flags themselves are refused by a caller that only knows V3 -
           which is what the V3 entry point hands to the same validator. */
        declaredPalette.choices=3;declaredPalette.choice_labels=kSwatches;
        Field v4Only=fromDeclared(declaredPalette);
        assert(validate(v4Only,config.size(),why,TC_COMPONENT_FIELD_KNOWN_FLAGS));
        assert(!validate(v4Only,config.size(),why,TC_COMPONENT_FIELD_V3_KNOWN_FLAGS)&&
               why.find("unknown flags")!=std::string::npos);
        assert(TC_COMPONENT_FIELD_V3_KNOWN_FLAGS!=TC_COMPONENT_FIELD_KNOWN_FLAGS);

        /* The marker is a presentation choice, so it is allowed on any kind. */
        Field marker=declared(TC_COMPONENT_FIELD_BOOL,0,1);
        marker.validationIcon=true;marker.flags=TC_COMPONENT_FIELD_VALIDATION_ICON;
        assert(validate(marker,config.size(),why));
    }

    /* The colour literals themselves: what a swatch declares and what the hex
       box beside it accepts. */
    {
        uint32_t packed=0;
        assert(parseColorLiteral("#ff8040",&packed)&&packed==UINT32_C(0xff4080ff));
        assert(parseColorLiteral("ff8040",&packed)&&packed==UINT32_C(0xff4080ff));
        assert(parseColorLiteral("0xff8040",&packed)&&packed==UINT32_C(0xff4080ff));
        assert(parseColorLiteral("#f84",&packed)&&packed==UINT32_C(0xff4488ff));
        assert(parseColorLiteral("#ff804080",&packed)&&packed==UINT32_C(0x804080ff));
        assert(parseColorLiteral("  #ff8040  ",&packed)&&packed==UINT32_C(0xff4080ff));
        assert(!parseColorLiteral("",&packed));
        assert(!parseColorLiteral("#ff80",&packed));
        assert(!parseColorLiteral("#gg8040",&packed));
        assert(!parseColorLiteral("#ff80400",&packed));
        assert(!parseColorLiteral("#ff8040zz",&packed));
        /* The hex box takes the packed word as well as a literal. */
        assert(parseColorHex("0xff4080ff",&packed)&&packed==UINT32_C(0xff4080ff));
        assert(parseColorHex("#ff8040",&packed)&&packed==UINT32_C(0xff4080ff));
        assert(!parseColorHex("",&packed));
        assert(!parseColorHex("0x1ff4080ff",&packed));
        assert(formatColorHex(UINT32_C(0xff4080ff))=="0xff4080ff");
    }

    /* Table flags: unknown bits are refused, and so is a staged table that also
       declares a custom row - two owners for one configuration blob. */
    {
        std::vector<Field> staged{declared(TC_COMPONENT_FIELD_BOOL,0,1),
                                  enumField(1,{"a","b"})};
        assert(validateTableFlags(0,staged,why));
        assert(validateTableFlags(TC_COMPONENT_EDITOR_TABLE_CONFIRM,staged,why));
        assert(!validateTableFlags(1u<<7,staged,why)&&why.find("unknown flags")!=std::string::npos);
        std::vector<Field> withCustom{declared(TC_COMPONENT_FIELD_CUSTOM,0,0)};
        assert(!validateTableFlags(TC_COMPONENT_EDITOR_TABLE_CONFIRM,withCustom,why)&&
               why.find("custom row")!=std::string::npos);
        assert(validateTableFlags(0,withCustom,why));
    }

    /* Table rules: a row that shares a line needs a row to share it with, and
       the rows that take the whole line end it. */
    {
        std::vector<Field> table;
        Field first=declared(TC_COMPONENT_FIELD_UINT,0,2);
        Field second=declared(TC_COMPONENT_FIELD_FLOAT,4,4);
        second.inlineWithPrevious=true;
        table={first,second};
        assert(validateTable(table,16,why));
        table[0].inlineWithPrevious=true;
        assert(!validateTable(table,16,why)&&why.find("first row")!=std::string::npos);
        table[0].inlineWithPrevious=false;
        table[0].fullRow=true;
        assert(!validateTable(table,16,why)&&why.find("whole line")!=std::string::npos);
        /* A radio group takes the whole line too, so nothing may share it. */
        Field group=enumField(1,{"a","b"});
        group.radio=true;
        Field plain=declared(TC_COMPONENT_FIELD_FLOAT,4,4);   /* a normal row after it */
        table={group,plain};
        assert(validateTable(table,16,why));
        Field afterGroup=declared(TC_COMPONENT_FIELD_UINT,8,1);
        afterGroup.inlineWithPrevious=true;
        table={group,afterGroup};
        assert(!validateTable(table,16,why)&&why.find("whole line")!=std::string::npos);
        /* Nor may a third row join a pair. */
        table={first,second};
        Field third=declared(TC_COMPONENT_FIELD_UINT,8,1);
        third.inlineWithPrevious=true;
        table.push_back(third);
        assert(!validateTable(table,16,why)&&why.find("already shares")!=std::string::npos);
    }

    /* The hexadecimal text a field shows and parses: padded to its own width so
       the digits do not jump around, and strict about what it accepts. */
    field=declared(TC_COMPONENT_FIELD_UINT,0,4);
    field.hex=true;field.bits=32;
    assert(validate(field,config.size(),why));
    assert(formatHex(field,0x10)=="0x00000010");
    assert(formatHex(field,0xdeadbeef)==std::string("0xdeadbeef"));
    field.bytes=2;field.bits=12;
    assert(validate(field,config.size(),why));
    assert(formatHex(field,0xabc)=="0x0abc");
    uint64_t parsed=0;
    assert(parseHex("0x1f40",&parsed)&&parsed==0x1f40);
    assert(parseHex("1F40",&parsed)&&parsed==0x1f40);
    assert(parseHex("  0X10  ",&parsed)&&parsed==0x10);
    assert(parseHex("0",&parsed)&&parsed==0);
    assert(!parseHex("",&parsed));
    assert(!parseHex("0x",&parsed));
    assert(!parseHex("0xzz",&parsed));
    assert(!parseHex("12 34",&parsed));
    assert(!parseHex("0x1234567890abcdef0",&parsed));   /* seventeen digits */
    /* A 64-bit field round-trips its whole range through the text form. */
    field=declared(TC_COMPONENT_FIELD_UINT,0,8);
    field.hex=true;
    assert(validate(field,config.size(),why));
    value.integer=static_cast<int64_t>(UINT64_C(0xffffffffffffffff));
    assert(apply(field,config.data(),config.size(),value,nullptr));
    assert(read(field,config.data(),config.size(),back));
    assert(formatHex(field,back.integer)==std::string("0xffffffffffffffff"));
    assert(parseHex(formatHex(field,back.integer),&parsed)&&parsed==~uint64_t(0));

    /* ---- the width-aware decimal float rows --------------------------- */
    field=declared(TC_COMPONENT_FIELD_FLOAT_WIDTH,0,2);
    assert(validate(field,config.size(),why));
    field=declared(TC_COMPONENT_FIELD_FLOAT_WIDTH,0,4);
    assert(validate(field,config.size(),why));
    field=declared(TC_COMPONENT_FIELD_FLOAT_WIDTH,0,8);
    assert(validate(field,config.size(),why));
    field=declared(TC_COMPONENT_FIELD_FLOAT_WIDTH,0,3);
    assert(!validate(field,config.size(),why)&&why.find("3 bytes")!=std::string::npos);
    /* The kind name in the loader's log says which width the row edits. */
    field=declared(TC_COMPONENT_FIELD_FLOAT_WIDTH,0,2);
    assert(std::string(kindName(field))=="float16");
    field.bytes=8;
    assert(std::string(kindName(field))=="float64");

    /* The text a row shows is the shortest spelling that parses back to the
       stored pattern - the same rule the Mod's own text layer uses on the board,
       so the drawer and the component agree. */
    field=declared(TC_COMPONENT_FIELD_FLOAT_WIDTH,0,2);
    assert(validate(field,config.size(),why));
    assert(formatFloatText(field,0x3C00)=="1");
    assert(formatFloatText(field,0x2E66)=="0.1");
    assert(formatFloatText(field,0x3555)=="0.3333");
    /* `%g` picks the scientific form once the exponent reaches the precision, so
       the largest binary16 reads "6.55e+04" here; what matters is that it parses
       back to 65504 (the board's own renderer spells it "65500"). */
    assert(formatFloatText(field,0x7BFF)=="6.55e+04");
    assert(formatFloatText(field,0xC000)=="-2");
    assert(formatFloatText(field,0x8000)=="-0");
    assert(formatFloatText(field,0x7C00)=="inf");
    assert(formatFloatText(field,0xFC00)=="-inf");
    assert(formatFloatText(field,0x7E00)=="nan");
    assert(formatFloatText(field,0x0001)=="6e-08");
    field.bytes=8;
    assert(validate(field,config.size(),why));
    assert(formatFloatText(field,static_cast<int64_t>(UINT64_C(0x3FF0000000000000)))=="1");
    assert(formatFloatText(field,static_cast<int64_t>(UINT64_C(0x3FB999999999999A)))=="0.1");
    assert(formatFloatText(field,static_cast<int64_t>(UINT64_C(0x7FEFFFFFFFFFFFFF)))=="1.7976931348623157e+308");
    assert(formatFloatText(field,static_cast<int64_t>(UINT64_C(0x7FF0000000000000)))=="inf");
    assert(formatFloatText(field,static_cast<int64_t>(UINT64_C(0x7FF8000000000000)))=="nan");
    assert(formatFloatText(field,static_cast<int64_t>(UINT64_C(0x8000000000000000)))=="-0");

    /* Typing is correctly rounded to the row's own width. */
    field.bytes=2;
    assert(parseFloatText(field,"0.1",&parsed,nullptr)&&parsed==0x2E66);
    assert(parseFloatText(field,"1",&parsed,nullptr)&&parsed==0x3C00);
    assert(parseFloatText(field,"1.5",&parsed,nullptr)&&parsed==0x3E00);
    assert(parseFloatText(field,"-0",&parsed,nullptr)&&parsed==0x8000);
    assert(parseFloatText(field,"inf",&parsed,nullptr)&&parsed==0x7C00);
    assert(parseFloatText(field,"nan",&parsed,nullptr)&&parsed==0x7E00);
    assert(parseFloatText(field,"  3.5  ",&parsed,nullptr)&&parsed==0x4300);
    /* A value too large for the width becomes its infinity, the way the board's
       own kernel would compute it. */
    assert(parseFloatText(field,"1e40",&parsed,nullptr)&&parsed==0x7C00);
    assert(parseFloatText(field,"1e-40",&parsed,nullptr)&&parsed==0x0000);
    /* Text that is not a number is refused, so the caller keeps the old bytes. */
    assert(!parseFloatText(field,"",&parsed,nullptr));
    assert(!parseFloatText(field,"   ",&parsed,nullptr));
    assert(!parseFloatText(field,"abc",&parsed,nullptr));
    assert(!parseFloatText(field,"1.2.3",&parsed,nullptr));
    assert(!parseFloatText(field,"3.5x",&parsed,nullptr));
    field.bytes=8;
    assert(parseFloatText(field,"0.1",&parsed,nullptr)&&
           parsed==UINT64_C(0x3FB999999999999A));
    assert(parseFloatText(field,"1e400",&parsed,nullptr)&&
           parsed==UINT64_C(0x7FF0000000000000));

    /* Every binary16 pattern survives text -> pattern unchanged: the row can show
       any value the format holds, including subnormals and every NaN. */
    field=declared(TC_COMPONENT_FIELD_FLOAT_WIDTH,0,2);
    for(uint32_t pattern=0;pattern<=0xFFFFu;++pattern){
        const std::string text=formatFloatText(field,static_cast<int64_t>(pattern));
        uint64_t parsed_pattern=0;
        assert(parseFloatText(field,text,&parsed_pattern,nullptr));
        if(pattern>=0x7C01u&&pattern<=0x7FFFu)continue;   /* a NaN payload collapses */
        if(pattern>=0xFC01u&&pattern<=0xFFFFu)continue;
        assert(parsed_pattern==pattern);
    }

    /* And the row round-trips through the configuration bytes, exactly like the
       hexadecimal one: what the box shows is what the instance holds. */
    field=declared(TC_COMPONENT_FIELD_FLOAT_WIDTH,4,2);
    assert(validate(field,config.size(),why));
    value.integer=0x2E66;
    const uint8_t neighbour=config[6];
    assert(apply(field,config.data(),config.size(),value,nullptr));
    assert(config[4]==0x66&&config[5]==0x2E&&config[6]==neighbour);
    assert(read(field,config.data(),config.size(),back));
    assert(back.integer==0x2E66&&back.text=="0.1");
    assert(display(field,back)=="0.1");
    field.bytes=8;
    assert(validate(field,config.size(),why));
    value.integer=static_cast<int64_t>(UINT64_C(0x3FF0000000000000));
    assert(apply(field,config.data(),config.size(),value,nullptr));
    assert(read(field,config.data(),config.size(),back)&&back.text=="1");

    std::cout<<"component editor ok\n";
    return 0;
}
