//  Copyright (C) 2017 The YaCo Authors
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <YaTypes.hpp>
#include "Ida.h"

#include "IDANativeExporter.hpp"

#include <YaToolObjectVersion.hpp>
#include <MultiplexerDelegatingVisitor.hpp>
#include "Logger.h"
#include "Yatools.h"
#include "../Helpers.h"

#include <string>
#include <iostream>
#include <set>
#include <chrono>
#include <regex>

#define LOG(LEVEL, FMT, ...) CONCAT(YALOG_, LEVEL)("IDANativeExporter", (FMT), ## __VA_ARGS__)

#ifdef __EA64__
#define EA_FMT  "%llx"
#define SEL_FMT "%lld"
#else
#define EA_FMT  "%x"
#define SEL_FMT "%d"
#endif

void IDANativeExporter::make_name(std::shared_ptr<YaToolObjectVersion> version, ea_t ea, bool is_in_func)
{
    const auto& name = version->get_name();
    auto flags = version->get_name_flags();
    if(!flags)
        flags = SN_CHECK;

    const auto reset_flags = SN_CHECK | (is_in_func ? SN_LOCAL : 0);
    const auto previous = get_true_name(ea);
    set_name(ea, "", reset_flags);
    if(name.empty() || IsDefaultName(make_string_ref(name)))
    {
        LOG(DEBUG, "make_name: 0x" EA_FMT " resetting name %s\n", ea, name.data());
        return;
    }

    const auto ok = set_name(ea, name.data(), flags | SN_NOWARN);
    if(ok)
        return;

    LOG(WARNING, "make_name: 0x" EA_FMT " unable to set name flags 0x%08x '%s'\n", ea, flags, name.data());
    set_name(ea, previous.c_str(), SN_CHECK | SN_NOWARN);
}

void IDANativeExporter::make_anterior_comment(ea_t address, const char* comment)
{
    tools.make_extra_comment(address, comment, E_PREV);
}

void IDANativeExporter::make_posterior_comment(ea_t address, const char* comment)
{
    tools.make_extra_comment(address, comment, E_NEXT);
}

void add_bookmark(ea_t ea, std::string comment_text)
{
    char buffer[1024];
    curloc loc;
    for(int i = 1; i < 1024; ++i)
    {
        const auto locea = loc.markedpos(&i);
        if(locea == BADADDR)
            break;

        LOG(DEBUG, "add_bookmark: 0x" EA_FMT " found bookmark[%d]\n", ea, i);
        if(locea != ea)
            continue;

        loc.markdesc(i, buffer, sizeof buffer);
        if(comment_text == buffer)
            continue;

        LOG(DEBUG, "add_bookmark: 0x" EA_FMT " bookmark[%d] = %s\n", ea, i, comment_text.data());
        loc.ea = ea;
        loc.x = 0;
        loc.y = 0;
        loc.lnnum = 0;
        loc.mark(i, comment_text.data(), comment_text.data());
    }
}

namespace
{
    std::string sanitize_comment_to_ascii(const std::string& comment)
    {
        return comment;
    }
}

void IDANativeExporter::make_comments(std::shared_ptr<YaToolObjectVersion> object_version, ea_t address)
{
    const auto current_comments = tools.get_comments_in_area(address, static_cast<ea_t>(address + object_version->get_size()));
    const auto new_comments = object_version->get_offset_comments();
    for(const auto& current_cmt : current_comments)
    {
        const auto comment_offset = current_cmt.first - address;
        for(const auto& one_comment : current_cmt.second)
        {
            const auto comment_type = one_comment.first;
            const auto& current_comment_text = one_comment.second;
            const auto it = new_comments.find(std::make_pair(static_cast<offset_t>(comment_offset), comment_type));
            if(it != new_comments.end() && it->second == current_comment_text)
                continue;
            tools.delete_comment_at_ea(address + current_cmt.first, comment_type);
        }
    }

    for(const auto& new_comment : new_comments)
    {
        const auto comment_offset = new_comment.first.first;
        const auto ea = static_cast<ea_t>(address + comment_offset);
        const auto comment_type = new_comment.first.second;
        const auto& comment_text = sanitize_comment_to_ascii(new_comment.second);
        LOG(DEBUG, "make_comments: 0x" EA_FMT " adding comment type %d\n", ea, comment_type);
        switch(comment_type)
        {
            case COMMENT_REPEATABLE:
                set_cmt(ea, comment_text.c_str(), 1);
                break;
            case COMMENT_NON_REPEATABLE:
                set_cmt(ea, comment_text.c_str(), 0);
                break;
            case COMMENT_ANTERIOR:
                make_anterior_comment(ea, comment_text.data());
                break;
            case COMMENT_POSTERIOR:
                make_posterior_comment(ea, comment_text.data());
                break;
            case COMMENT_BOOKMARK:
                add_bookmark(ea, comment_text);
                break;
            default:
                LOG(ERROR, "make_comments: 0x" EA_FMT " unknown comment type %d\n", ea, comment_type);
                break;
        }
    }
}

namespace
{
// use a macro & ensure compiler statically check sscanf...
#define MAKE_TO_TYPE_FUNCTION(NAME, TYPE, FMT)\
TYPE NAME(const char* value)\
{\
    TYPE reply = {};\
    sscanf(value, FMT, &reply);\
    return reply;\
}

MAKE_TO_TYPE_FUNCTION(to_ea,      ea_t,             EA_FMT);
MAKE_TO_TYPE_FUNCTION(to_uchar,   uchar,            "%hhd");
MAKE_TO_TYPE_FUNCTION(to_ushort,  ushort,           "%hd");
MAKE_TO_TYPE_FUNCTION(to_int,     int,              "%d");
MAKE_TO_TYPE_FUNCTION(to_sel,     sel_t,            SEL_FMT);
MAKE_TO_TYPE_FUNCTION(to_bgcolor, bgcolor_t,        "%d");
MAKE_TO_TYPE_FUNCTION(to_yaid,    YaToolObjectId,   "%llx");

template<typename T>
int find_int(const T& data, const char* key)
{
    const auto it = data.find(key);
    if(it == data.end())
        return 0;
    return to_int(it->second.data());
}

bool check_segment(ea_t ea, ea_t start, ea_t end)
{
    const auto segment = getseg(ea);
    if(!segment)
        return false;
    return segment->startEA == start && segment->endEA == end;
}

bool add_seg(ea_t start, ea_t end, ea_t base, int bitness, int align, int comb)
{
    segment_t seg;
    seg.startEA = start;
    seg.endEA = end;
    seg.sel = setup_selector(base);
    seg.bitness = static_cast<uchar>(bitness);
    seg.align = static_cast<uchar>(align);
    seg.comb = static_cast<uchar>(comb);
    return add_segm_ex(&seg, "", "", ADDSEG_NOSREG);
}

enum SegAttribute
{
    SEG_ATTR_START,
    SEG_ATTR_END,
    SEG_ATTR_BASE,
    SEG_ATTR_ALIGN,
    SEG_ATTR_COMB,
    SEG_ATTR_PERM,
    SEG_ATTR_BITNESS,
    SEG_ATTR_FLAGS,
    SEG_ATTR_SEL,
    SEG_ATTR_ES,
    SEG_ATTR_CS,
    SEG_ATTR_SS,
    SEG_ATTR_DS,
    SEG_ATTR_FS,
    SEG_ATTR_GS,
    SEG_ATTR_TYPE,
    SEG_ATTR_COLOR,
    SEG_ATTR_COUNT,
};

// copied from _SEGATTRMAP in idc.py...
enum RegAttribute
{
    REG_ATTR_ES = 0,
    REG_ATTR_CS = 1,
    REG_ATTR_SS = 2,
    REG_ATTR_DS = 3,
    REG_ATTR_FS = 4,
    REG_ATTR_GS = 5,
};

const char g_seg_attributes[][12] =
{
    "start_ea",
    "end_ea",
    "org_base",
    "align",
    "comb",
    "perm",
    "bitness",
    "flags",
    "sel",
    "es",
    "cs",
    "ss",
    "ds",
    "fs",
    "gs",
    "type",
    "color",
};

static_assert(COUNT_OF(g_seg_attributes) == SEG_ATTR_COUNT, "invalid number of g_seg_attributes entries");

SegAttribute get_segment_attribute(const char* value)
{
    for(size_t i = 0; i < COUNT_OF(g_seg_attributes); ++i)
        if(!strcmp(g_seg_attributes[i], value))
            return static_cast<SegAttribute>(i);
    return SEG_ATTR_COUNT;
}

void set_segment_attribute(segment_t* seg, const char* key, const char* value)
{
    switch(get_segment_attribute(key))
    {
        case SEG_ATTR_START:
            seg->startEA = to_ea(value);
            break;

        case SEG_ATTR_END:
            seg->endEA = to_ea(value);
            break;

        case SEG_ATTR_BASE:
            set_segm_base(seg, to_ea(value));
            break;

        case SEG_ATTR_ALIGN:
            seg->align = to_uchar(value);
            break;

        case SEG_ATTR_COMB:
            seg->comb = to_uchar(value);
            break;

        case SEG_ATTR_PERM:
            seg->perm = to_uchar(value);
            break;

        case SEG_ATTR_BITNESS:
            set_segm_addressing(seg, to_int(value));
            break;

        case SEG_ATTR_FLAGS:
            seg->flags = to_ushort(value);
            break;

        case SEG_ATTR_SEL:
            seg->sel = to_sel(value);
            break;

        case SEG_ATTR_ES:
            seg->defsr[REG_ATTR_ES] = to_sel(value);
            break;

        case SEG_ATTR_CS:
            seg->defsr[REG_ATTR_CS] = to_sel(value);
            break;

        case SEG_ATTR_SS:
            seg->defsr[REG_ATTR_SS] = to_sel(value);
            break;

        case SEG_ATTR_DS:
            seg->defsr[REG_ATTR_DS] = to_sel(value);
            break;

        case SEG_ATTR_FS:
            seg->defsr[REG_ATTR_FS] = to_sel(value);
            break;

        case SEG_ATTR_GS:
            seg->defsr[REG_ATTR_GS] = to_sel(value);
            break;

        case SEG_ATTR_TYPE:
            seg->type = to_uchar(value);
            break;

        case SEG_ATTR_COLOR:
            seg->color = to_bgcolor(value);
            break;

        case SEG_ATTR_COUNT:
            break;
    }
}
}

void IDANativeExporter::make_segment(std::shared_ptr<YaToolObjectVersion> version, ea_t ea)
{
    const auto size = version->get_size();
    const auto name = version->get_name();
    const auto attributes = version->get_attributes();
    const auto end  = static_cast<ea_t>(ea + size);

    if(!check_segment(ea, ea, end))
    {
        const auto align = find_int(attributes, "align");
        const auto comb = find_int(attributes, "comb");
        const auto ok = add_seg(ea, end, 0, 1, align, comb);
        if(!ok)
            LOG(ERROR, "make_segment: 0x" EA_FMT " unable to add segment [0x" EA_FMT ", 0x" EA_FMT "] align:%d comb:%d\n", ea, ea, end, align, comb);
    }

    auto seg = getseg(ea);
    if(!seg)
    {
        LOG(ERROR, "make_segment: 0x" EA_FMT " unable to get segment\n", ea);
        return;
    }

    if(!name.empty())
    {
        const auto ok = set_segm_name(seg, name.data());
        if(!ok)
            LOG(ERROR, "make_segment: 0x" EA_FMT " unable to set name %s\n", ea, name.data());
    }

    const auto is_readonly = [](const std::string& key)
    {
        static const char read_only_attributes[][12] =
        {
            "start_ea",
            "end_ea",
            "sel",
        };
        for(const auto& it : read_only_attributes)
            if(key == it)
                return true;
        return false;
    };

    bool updated = false;
    for(const auto& p : attributes)
    {
        if(is_readonly(p.first))
            continue;
        set_segment_attribute(seg, p.first.data(), p.second.data());
        updated = true;
    }
    if(!updated)
        return;

    const auto ok = seg->update();
    if(!ok)
        LOG(ERROR, "make_segment: 0x" EA_FMT " unable to update segment\n", ea);
}

void IDANativeExporter::make_segment_chunk(std::shared_ptr<YaToolObjectVersion> version, ea_t)
{
    // TODO : now that we have enough precision, we could delete elements
    // that are in the base but not in our segment_chunk
    std::vector<uint8_t> buffer;
    for(const auto& it : version->get_blobs())
    {
        const auto offset = static_cast<ea_t>(it.first);
        const auto& data = it.second;
        buffer.resize(data.size());
        auto ok = get_many_bytes(offset, &buffer[0], data.size());
        if(!ok)
        {
            LOG(ERROR, "make_segment_chunk: 0x" EA_FMT " unable to read %d bytes\n", offset, data.size());
            continue;
        }
        if(data == buffer)
            continue;

        // put_many_bytes does not return any error code...
        put_many_bytes(offset, &data[0], data.size());
        ok = get_many_bytes(offset, &buffer[0], data.size());
        if(!ok || data != buffer)
            LOG(ERROR, "make_segment_chunk: 0x" EA_FMT " unable to write %d bytes\n", offset, data.size());
    }
}

void IDANativeExporter::set_struct_id(YaToolObjectId id, uint64_t struct_id)
{
    struct_ids[id] = struct_id;
}

namespace
{
const std::regex r_trailing_identifier  {"\\s*<?[a-zA-Z_0-9]+>?\\s*$"};     // match c/c++ identifiers
const std::regex r_type_id              {"/\\*%(.+?)#([A-F0-9]{16})%\\*/"}; // match yaco ids /*%name:ID%*/
const std::regex r_trailing_comma       {"\\s*;\\s*$"};                     // match trailing ;
const std::regex r_trailing_whitespace  {"\\s+$"};                          // match trailing whitespace
const std::regex r_leading_whitespace   {"^\\s+"};                          // match leading whitespace
const std::regex r_trailing_pointer     {"\\*\\s*$"};                       // match trailing *

void replace_inline(std::string& value, const std::string& pattern, const std::string& replace)
{
    size_t pos = 0;
    while(true)
    {
        pos = value.find(pattern, pos);
        if(pos == std::string::npos)
            break;

        value.replace(pos, pattern.size(), replace);
        pos += replace.size();
    }
}
}

std::string IDANativeExporter::patch_prototype(const std::string& src, ea_t ea)
{
    // remove/patch struct ids
    auto dst = src;
    qstring buffer;
    for(std::sregex_iterator it = {src.begin(), src.end(), r_type_id}, end; it != end; ++it)
    {
        const auto id = it->str(2);
        const auto sid = struct_ids.find(to_yaid(id.data()));
        const auto name = it->str(1);
        // always remove special struct comment
        replace_inline(dst, "/*%" + name + "#" + id + "%*/", "");
        if(sid == struct_ids.end())
        {
            LOG(WARNING, "make_prototype: 0x" EA_FMT " unknown struct %s id %s\n", ea, name.data(), id.data());
            continue;
        }
        const auto tid = static_cast<tid_t>(sid->second);
        get_struc_name(&buffer, tid);
        // replace struct name with new name
        replace_inline(dst, name, buffer.c_str());
    }

    // remove trailing whitespace
    dst = std::regex_replace(dst, r_trailing_whitespace, "");
    return dst;
}

namespace
{
    tinfo_t make_simple_type(type_t type)
    {
        tinfo_t tif;
        tif.create_simple_type(type);
        return tif;
    }

    tinfo_t try_find_type(const char* value)
    {
        tinfo_t tif;
        std::string decl = value;
        auto ok = parse_decl2(idati, (decl + ";").data(), nullptr, &tif, PT_SIL);
        if(ok)
            return tif;

        tif.clear();
        ok = tif.get_named_type(idati, value);
        if(ok)
            return tif;

        tif.clear();
        return tif;
    }

    size_t remove_pointers(std::string* value)
    {
        size_t count = 0;
        while(true)
        {
            auto dst = std::regex_replace(*value, r_trailing_pointer, "");
            dst = std::regex_replace(dst, r_trailing_whitespace, "");
            if(dst == *value)
                break;
            *value = dst;
            count++;
         }
        return count;
    }

    tinfo_t add_back_pointers(const tinfo_t& tif, size_t num_pointers)
    {
        tinfo_t work = tif;
        for(size_t i = 0; i < num_pointers; ++i)
        {
            tinfo_t next;
            next.create_ptr(work);
            work = next;
        }
        return work;
    }

    tinfo_t find_single_type(const std::string& input)
    {
        // special case 'void' type because ida doesn't want to parse it...
        if(input == "void")
            return make_simple_type(BT_VOID);

        std::string value = input;
        auto tif = try_find_type(value.data());
        if(!tif.empty())
            return tif;

        value = std::regex_replace(value, r_trailing_comma, "");
        value = std::regex_replace(value, r_trailing_whitespace, "");
        auto num_pointers = remove_pointers(&value);
        tif = try_find_type(value.data());
        if(!tif.empty())
            return add_back_pointers(tif, num_pointers);

        // remove left-most identifier, which is possibly a variable name
        value = std::regex_replace(value, r_trailing_identifier, "");
        value = std::regex_replace(value, r_trailing_whitespace, "");
        num_pointers = remove_pointers(&value);
        tif = try_find_type(value.data());
        if(!tif.empty())
            return add_back_pointers(tif, num_pointers);

        return tinfo_t();
    }

    const std::regex r_varargs {"\\s*\\.\\.\\.\\s*\\)$"};

    cm_t get_calling_convention(const std::string& value, const std::string& args)
    {
        const auto has_varargs = std::regex_match(args, r_varargs);
        if(value == "__cdecl")
            return has_varargs ? CM_CC_ELLIPSIS : CM_CC_CDECL;
        if(value == "__stdcall")
            return CM_CC_STDCALL;
        if(value == "__pascal")
            return CM_CC_PASCAL;
        if(value == "__thiscall")
            return CM_CC_THISCALL;
        if(value == "__usercall")
            return has_varargs ? CM_CC_SPECIALE : CM_CC_SPECIAL;
        return CM_CC_UNKNOWN;
    }

    std::vector<std::string> split_args(const std::string& value)
    {
        std::vector<std::string> args;
        int in_templates = 0;
        int in_parens = 0;
        int in_comments = 0;
        size_t previous = 0;
        char cprev = 0;

        const auto add_arg = [&](size_t i)
        {
            auto arg = value.substr(previous, i - previous);
            arg = std::regex_replace(arg, r_leading_whitespace, "");
            arg = std::regex_replace(arg, r_trailing_whitespace, "");
            args.emplace_back(arg);
            previous = i + 1;
        };

        // ugly & broken way to determine where to split on ','
        for(size_t i = 0, end = value.size(); i < end; ++i)
        {
            const auto c = value[i];

            in_templates += c == '<';
            in_parens += c == '(';
            in_comments += c == '*' && cprev == '/';

            in_templates -= c == '>';
            in_parens -= c == ')';
            in_comments -= c == '/' && cprev == '*';

            // we have a ','
            cprev = c;
            if(c != ',')
                continue;
            if(in_templates || in_parens || in_comments)
                continue;
            add_arg(i);
        }
        if(!value.empty())
            add_arg(value.size());

        return args;
    }

    const std::regex r_function_definition  {"^(.+?)\\s*(__\\w+)\\s+sub\\((.*)\\)$"};

    tinfo_t find_type(ea_t ea, const std::string& input);

    tinfo_t try_find_type(ea_t ea, const std::string& input)
    {
        auto tif = find_single_type(input);
        if(!tif.empty())
            return tif;

        std::smatch match;
        auto ok = std::regex_match(input, match, r_function_definition);
        if(!ok)
            return tinfo_t();

        // we have a function definition
        const auto return_type = match.str(1);
        const auto calling_convention = match.str(2);
        const auto args = match.str(3);

        func_type_data_t ft;
        ft.rettype = find_type(ea, return_type);
        if(ft.rettype.empty())
            return tinfo_t();

        ft.cc = get_calling_convention(calling_convention, args);
        for(const auto& token : split_args(args))
        {
            funcarg_t arg;
            arg.type = find_type(ea, token);
            if(arg.type.empty())
                return tinfo_t();

            // FIXME try to parse argument name, it often work but is fundamentally broken
            std::string argname;
            const auto stripped = std::regex_replace(token, r_trailing_identifier, "");
            tif = find_type(ea, "typedef " + token + " a b");
            if(tif.empty())
                argname = token.substr(stripped.size());
            argname = std::regex_replace(argname, r_leading_whitespace, "");
            argname = std::regex_replace(argname, r_trailing_whitespace, "");

            arg.name = {argname.data(), argname.size()};
            ft.push_back(arg);
        }

        tif.clear();
        ok = tif.create_func(ft);
        if(!ok)
            return tinfo_t();

        return tif;
    }

    tinfo_t find_type(ea_t ea, const std::string& input)
    {
        tinfo_t tif = try_find_type(ea, input);
        if(tif.empty())
            LOG(ERROR, "find_type: 0x" EA_FMT " unable to guess type for %s\n", ea, input.data());
        return tif;
    }

    template<typename T>
    bool try_set_type(IDANativeExporter& exporter, ea_t ea, const std::string& value, const T& operand)
    {
        if(value.empty())
            return false;

        const auto patched = exporter.patch_prototype(value, ea);
        const auto tif = find_type(ea, patched.data());
        const auto ok = operand(tif);
        if(!ok)
            LOG(ERROR, "set_type: 0x" EA_FMT " unable to set type %s\n", ea, patched.data());
        return ok;
    }
}

bool IDANativeExporter::set_type(ea_t ea, const std::string& value)
{
    return try_set_type(*this, ea, value, [&](const tinfo_t& tif)
    {
        return apply_tinfo2(ea, tif, TINFO_DEFINITE);
    });
}

bool IDANativeExporter::set_struct_member_type(ea_t ea, const std::string& value)
{
    return try_set_type(*this, ea, value, [&](const tinfo_t& tif)
    {
        struc_t* s = nullptr;
        auto* m = get_member_by_id(ea, &s);
        return s && m && set_member_tinfo2(s, m, 0, tif, 0);
    });
}
