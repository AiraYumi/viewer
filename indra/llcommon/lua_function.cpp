/**
 * @file   lua_function.cpp
 * @author Nat Goodspeed
 * @date   2024-02-05
 * @brief  Implementation for lua_function.
 * 
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Copyright (c) 2024, Linden Research, Inc.
 * $/LicenseInfo$
 */

// Precompiled header
#include "linden_common.h"
// associated header
#include "lua_function.h"
// STL headers
// std headers
#include <algorithm>
#include <exception>
#include <iomanip>                  // std::quoted
#include <map>
#include <memory>                   // std::unique_ptr
#include <typeinfo>
// external library headers
// other Linden headers
#include "fsyspath.h"
#include "hexdump.h"
#include "lleventcoro.h"
#include "llsd.h"
#include "llsdutil.h"
#include "lualistener.h"
#include "stringize.h"

const S32 INTERRUPTS_MAX_LIMIT = 20000;
const S32 INTERRUPTS_SUSPEND_LIMIT = 100;

#define lua_register(L, n, f) (lua_pushcfunction(L, (f), n), lua_setglobal(L, (n)))
#define lua_rawlen lua_objlen

/*****************************************************************************
*   luau namespace
*****************************************************************************/
namespace
{
    // can't specify free function free() as a unique_ptr deleter
    struct freer
    {
        void operator()(void* ptr){ free(ptr); }
    };
} // anonymous namespace

int lluau::dostring(lua_State* L, const std::string& desc, const std::string& text)
{
    auto r = loadstring(L, desc, text);
    if (r != LUA_OK)
        return r;

    // It's important to pass LUA_MULTRET as the expected number of return
    // values: if we pass any fixed number, we discard any returned values
    // beyond that number.
    return lua_pcall(L, 0, LUA_MULTRET, 0);
}

int lluau::loadstring(lua_State *L, const std::string &desc, const std::string &text)
{
    size_t bytecodeSize = 0;
    // The char* returned by luau_compile() must be freed by calling free().
    // Use unique_ptr so the memory will be freed even if luau_load() throws.
    std::unique_ptr<char[], freer> bytecode{
        luau_compile(text.data(), text.length(), nullptr, &bytecodeSize)};
    return luau_load(L, desc.data(), bytecode.get(), bytecodeSize, 0);
}

fsyspath lluau::source_path(lua_State* L)
{
    //Luau lua_Debug and lua_getinfo() are different compared to default Lua:
    //see https://github.com/luau-lang/luau/blob/80928acb92d1e4b6db16bada6d21b1fb6fa66265/VM/include/lua.h
    lua_Debug ar;
    lua_getinfo(L, 1, "s", &ar);
    return ar.source;
}

void lluau::set_interrupts_counter(lua_State *L, S32 counter)
{
    luaL_checkstack(L, 2, nullptr);
    lua_pushstring(L, "_INTERRUPTS");
    lua_pushinteger(L, counter);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

void lluau::check_interrupts_counter(lua_State* L)
{
    luaL_checkstack(L, 1, nullptr);
    lua_pushstring(L, "_INTERRUPTS");
    lua_rawget(L, LUA_REGISTRYINDEX);
    S32 counter = lua_tointeger(L, -1);
    lua_pop(L, 1);

    lluau::set_interrupts_counter(L, ++counter);
    if (counter > INTERRUPTS_MAX_LIMIT) 
    {
        lluau::error(L, "Possible infinite loop, terminated.");
    }
    else if (counter % INTERRUPTS_SUSPEND_LIMIT == 0) 
    {
        LL_DEBUGS("Lua") << LLCoros::getName() << " suspending at " << counter << " interrupts"
                         << LL_ENDL;
        llcoro::suspend();
    }
}

/*****************************************************************************
*   Lua <=> C++ conversions
*****************************************************************************/
std::string lua_tostdstring(lua_State* L, int index)
{
    size_t len;
    const char* strval{ lua_tolstring(L, index, &len) };
    return { strval, len };
}

void lua_pushstdstring(lua_State* L, const std::string& str)
{
    luaL_checkstack(L, 1, nullptr);
    lua_pushlstring(L, str.c_str(), str.length());
}

// By analogy with existing lua_tomumble() functions, return an LLSD object
// corresponding to the Lua object at stack index 'index' in state L.
// This function assumes that a Lua caller is fully aware that they're trying
// to call a viewer function. In other words, the caller must specifically
// construct Lua data convertible to LLSD.
//
// For proper error handling, we REQUIRE that the Lua runtime be compiled as
// C++ so errors are raised as C++ exceptions rather than as longjmp() calls:
// http://www.lua.org/manual/5.4/manual.html#4.4
// "Internally, Lua uses the C longjmp facility to handle errors. (Lua will
// use exceptions if you compile it as C++; search for LUAI_THROW in the
// source code for details.)"
// Some blocks within this function construct temporary C++ objects in the
// expectation that these objects will be properly destroyed even if code
// reached by that block raises a Lua error.
LLSD lua_tollsd(lua_State* L, int index)
{
    switch (lua_type(L, index))
    {
    case LUA_TNONE:
        // Should LUA_TNONE be an error instead of returning isUndefined()?
    case LUA_TNIL:
        return {};

    case LUA_TBOOLEAN:
        return bool(lua_toboolean(L, index));

    case LUA_TNUMBER:
    {
        // Vanilla Lua supports lua_tointegerx(), which tells the caller
        // whether the number at the specified stack index is or is not an
        // integer. Apparently the function exists but does not work right in
        // Luau: it reports even non-integer numbers as integers.
        // Instead, check if integer truncation leaves the number intact.
        lua_Number  numval{ lua_tonumber(L, index) };
        lua_Integer intval{ narrow(numval) };
        if (lua_Number(intval) == numval)
        {
            return LLSD::Integer(intval);
        }
        else
        {
            return numval;
        }
    }

    case LUA_TSTRING:
        return lua_tostdstring(L, index);

    case LUA_TUSERDATA:
    {
        LLSD::Binary binary(lua_rawlen(L, index));
        std::memcpy(binary.data(), lua_touserdata(L, index), binary.size());
        return binary;
    }

    case LUA_TTABLE:
    {
        // A Lua table correctly constructed to convert to LLSD will have
        // either consecutive integer keys starting at 1, which we represent
        // as an LLSD array (with Lua key 1 at C++ index 0), or will have
        // all string keys.
        //
        // In the belief that Lua table traversal skips "holes," that is, it
        // doesn't report any key/value pair whose value is nil, we allow a
        // table with integer keys >= 1 but with "holes." This produces an
        // LLSD array with isUndefined() entries at unspecified keys. There
        // would be no other way for a Lua caller to construct an
        // isUndefined() LLSD array entry. However, to guard against crazy int
        // keys, we forbid gaps larger than a certain size: crazy int keys
        // could result in a crazy large contiguous LLSD array.
        //
        // Possible looseness could include:
        // - A mix of integer and string keys could produce an LLSD map in
        //   which the integer keys are converted to string. (Key conversion
        //   must be performed in C++, not Lua, to avoid confusing
        //   lua_next().)
        // - However, since in Lua t[0] and t["0"] are distinct table entries,
        //   do not consider converting numeric string keys to int to return
        //   an LLSD array.
        // But until we get more experience with actual Lua scripts in
        // practice, let's say that any deviation is a Lua coding error.
        // An important property of the strict definition above is that most
        // conforming data blobs can make a round trip across the language
        // boundary and still compare equal. A non-conforming data blob would
        // lose that property.
        // Known exceptions to round trip identity:
        // - Empty LLSD map and empty LLSD array convert to empty Lua table.
        //   But empty Lua table converts to isUndefined() LLSD object.
        // - LLSD::Real with integer value returns as LLSD::Integer.
        // - LLSD::UUID, LLSD::Date and LLSD::URI all convert to Lua string,
        //   and so return as LLSD::String.
        // - Lua does not store any table key whose value is nil. An LLSD
        //   array with isUndefined() entries produces a Lua table with
        //   "holes" in the int key sequence; this converts back to an LLSD
        //   array containing corresponding isUndefined() entries -- except
        //   when one or more of the final entries isUndefined(). These are
        //   simply dropped, producing a shorter LLSD array than the original.
        // - For the same reason, any keys in an LLSD map whose value
        //   isUndefined() are simply discarded in the converted Lua table.
        //   This converts back to an LLSD map lacking those keys.
        // - If it's important to preserve the original length of an LLSD
        //   array whose final entries are undefined, or the full set of keys
        //   for an LLSD map some of whose values are undefined, store an
        //   LLSD::emptyArray() or emptyMap() instead. These will be
        //   represented in Lua as empty table, which should convert back to
        //   undefined LLSD. Naturally, though, those won't survive a second
        //   round trip.

        // This is the most important of the luaL_checkstack() calls because a
        // deeply nested Lua structure will enter this case at each level, and
        // we'll need another 2 stack slots to traverse each nested table.
        luaL_checkstack(L, 2, nullptr);
        // BEFORE we push nil to initialize the lua_next() traversal, convert
        // 'index' to absolute! Our caller might have passed a relative index;
        // we do, below: lua_tollsd(L, -1). If 'index' is -1, then when we
        // push nil, what we find at index -1 is nil, not the table!
        index = lua_absindex(L, index);
        lua_pushnil(L);             // first key
        if (! lua_next(L, index))
        {
            // it's a table, but the table is empty -- no idea if it should be
            // modeled as empty array or empty map -- return isUndefined(),
            // which can be consumed as either
            return {};
        }
        // key is at stack index -2, value at index -1
        // from here until lua_next() returns 0, have to lua_pop(2) if we
        // return early
        LuaPopper popper(L, 2);
        // Remember the type of the first key
        auto firstkeytype{ lua_type(L, -2) };
        switch (firstkeytype)
        {
        case LUA_TNUMBER:
        {
            // First Lua key is a number: try to convert table to LLSD array.
            // This is tricky because we don't know in advance the size of the
            // array. The Lua reference manual says that lua_rawlen() is the
            // same as the length operator '#'; but the length operator states
            // that it might stop at any "hole" in the subject table.
            // Moreover, the Lua next() function (and presumably lua_next())
            // traverses a table in unspecified order, even for numeric keys
            // (emphasized in the doc).
            // Make a preliminary pass over the whole table to validate and to
            // collect keys.
            std::vector<LLSD::Integer> keys;
            // Try to determine the length of the table. If the length
            // operator is truthful, avoid allocations while we grow the keys
            // vector. Even if it's not, we can still grow the vector, albeit
            // a little less efficiently.
            keys.reserve(lua_objlen(L, index));
            do
            {
                auto arraykeytype{ lua_type(L, -2) };
                switch (arraykeytype)
                {
                case LUA_TNUMBER:
                {
                    int isint;
                    lua_Integer intkey{ lua_tointegerx(L, -2, &isint) };
                    if (! isint)
                    {
                        // key isn't an integer - this doesn't fit our LLSD
                        // array constraints
                        return lluau::error(L, "Expected integer array key, got %f instead",
                                            lua_tonumber(L, -2));
                    }
                    if (intkey < 1)
                    {
                        return lluau::error(L, "array key %d out of bounds", int(intkey));
                    }

                    keys.push_back(LLSD::Integer(intkey));
                    break;
                }

                case LUA_TSTRING:
                    // break out strings specially to report the value
                    return lluau::error(L, "Cannot convert string array key '%s' to LLSD",
                                        lua_tostring(L, -2));

                default:
                    return lluau::error(L, "Cannot convert %s array key to LLSD",
                                        lua_typename(L, arraykeytype));
                }

                // remove value, keep key for next iteration
                lua_pop(L, 1);
            } while (lua_next(L, index) != 0);
            popper.disarm();
            // Table keys are all integers: are they reasonable integers?
            // Arbitrary max: may bite us, but more likely to protect us
            const size_t array_max{ 10000 };
            if (keys.size() > array_max)
            {
                return lluau::error(L, "Conversion from Lua to LLSD array limited to %d entries",
                                    int(array_max));
            }
            // We know the smallest key is >= 1. Check the largest. We also
            // know the vector is NOT empty, else we wouldn't have gotten here.
            std::sort(keys.begin(), keys.end());
            LLSD::Integer highkey = *keys.rbegin();
            if ((highkey - LLSD::Integer(keys.size())) > 100)
            {
                // Looks like we've gone beyond intentional array gaps into
                // crazy key territory.
                return lluau::error(L, "Gaps in Lua table too large for conversion to LLSD array");
            }
            // right away expand the result array to the size we'll need
            LLSD result{ LLSD::emptyArray() };
            result[highkey - 1] = LLSD();
            // Traverse the table again, and this time populate result array.
            lua_pushnil(L);         // first key
            while (lua_next(L, index))
            {
                // key at stack index -2, value at index -1
                // We've already validated lua_tointegerx() for each key.
                auto key{ lua_tointeger(L, -2) };
                // Don't forget to subtract 1 from Lua key for LLSD subscript!
                result[LLSD::Integer(key) - 1] = lua_tollsd(L, -1);
                // remove value, keep key for next iteration
                lua_pop(L, 1);
            }
            return result;
        }

        case LUA_TSTRING:
        {
            // First Lua key is a string: try to convert table to LLSD map
            LLSD result{ LLSD::emptyMap() };
            do
            {
                auto mapkeytype{ lua_type(L, -2) };
                if (mapkeytype != LUA_TSTRING)
                {
                    return lluau::error(L, "Cannot convert %s map key to LLSD",
                                        lua_typename(L, mapkeytype));
                }

                auto key{ lua_tostdstring(L, -2) };
                result[key] = lua_tollsd(L, -1);
                // remove value, keep key for next iteration
                lua_pop(L, 1);
            } while (lua_next(L, index) != 0);
            popper.disarm();
            return result;
        }

        default:
            // First Lua key isn't number or string: sorry
            return lluau::error(L, "Cannot convert %s table key to LLSD",
                                lua_typename(L, firstkeytype));
        }
    }

    default:
        // Other Lua entities (e.g. function, C function, light userdata,
        // thread, userdata) are not convertible to LLSD, indicating a coding
        // error in the caller.
        return lluau::error(L, "Cannot convert type %s to LLSD", luaL_typename(L, index));
    }
}

// By analogy with existing lua_pushmumble() functions, push onto state L's
// stack a Lua object corresponding to the passed LLSD object.
void lua_pushllsd(lua_State* L, const LLSD& data)
{
    // might need 2 slots for array or map
    luaL_checkstack(L, 2, nullptr);
    switch (data.type())
    {
    case LLSD::TypeUndefined:
        lua_pushnil(L);
        break;

    case LLSD::TypeBoolean:
        lua_pushboolean(L, data.asBoolean());
        break;

    case LLSD::TypeInteger:
        lua_pushinteger(L, data.asInteger());
        break;

    case LLSD::TypeReal:
        lua_pushnumber(L, data.asReal());
        break;

    case LLSD::TypeBinary:
    {
        auto binary{ data.asBinary() };
        std::memcpy(lua_newuserdata(L, binary.size()),
                    binary.data(), binary.size());
        break;
    }

    case LLSD::TypeMap:
    {
        // push a new table with space for our non-array keys
        lua_createtable(L, 0, data.size());
        for (const auto& pair: llsd::inMap(data))
        {
            // push value -- so now table is at -2, value at -1
            lua_pushllsd(L, pair.second);
            // pop value, assign to table[key]
            lua_setfield(L, -2, pair.first.c_str());
        }
        break;
    }

    case LLSD::TypeArray:
    {
        // push a new table with space for array entries
        lua_createtable(L, data.size(), 0);
        lua_Integer key{ 0 };
        for (const auto& item: llsd::inArray(data))
        {
            // push new array value: table at -2, value at -1
            lua_pushllsd(L, item);
            // pop value, assign table[key] = value
            lua_rawseti(L, -2, ++key);
        }
        break;
    }

    case LLSD::TypeString:
    case LLSD::TypeUUID:
    case LLSD::TypeDate:
    case LLSD::TypeURI:
    default:
    {
        lua_pushstdstring(L, data.asString());
        break;
    }
    }
}

/*****************************************************************************
*   LuaState class
*****************************************************************************/
LuaState::LuaState(script_finished_fn cb):
    mCallback(cb),
    mState(nullptr)
{
    initLuaState();
}

void LuaState::initLuaState() 
{
    if (mState)
    {
        lua_close(mState);
    }
    mState = luaL_newstate();
    luaL_openlibs(mState);
    LuaFunction::init(mState);
    // Try to make print() write to our log.
    lua_register(mState, "print", LuaFunction::get("print_info"));
    // We don't want to have to prefix require().
    lua_register(mState, "require", LuaFunction::get("require"));
}

LuaState::~LuaState()
{
    // Did somebody call obtainListener() on this LuaState?
    // That is, is there a LuaListener key in its registry?
    LuaListener::destruct(getListener());

    lua_close(mState);

    if (mCallback)
    {
        // mError potentially set by previous checkLua() call(s)
        mCallback(mError);
    }    
}

bool LuaState::checkLua(const std::string& desc, int r)
{
    if (r != LUA_OK)
    {
        mError = lua_tostring(mState, -1);
        lua_pop(mState, 1);

        LL_WARNS() << desc << ": " << mError << LL_ENDL;
        return false;
    }
    return true;
}

std::pair<int, LLSD> LuaState::expr(const std::string& desc, const std::string& text)
{
    lluau::set_interrupts_counter(mState, 0);

    lua_callbacks(mState)->interrupt = [](lua_State *L, int gc)
    {
        // skip if we're interrupting only for garbage collection
        if (gc >= 0)
            return;

        LLCoros::checkStop();
        lluau::check_interrupts_counter(L);
    };

    if (! checkLua(desc, lluau::dostring(mState, desc, text)))
    {
        LL_WARNS("Lua") << desc << " error: " << mError << LL_ENDL;
        return { -1, mError };
    }

    // here we believe there was no error -- did the Lua fragment leave
    // anything on the stack?
    std::pair<int, LLSD> result{ lua_gettop(mState), {} };
    LL_INFOS("Lua") << desc << " done, " << result.first << " results." << LL_ENDL;
    if (result.first)
    {
        // aha, at least one entry on the stack!
        if (result.first == 1)
        {
            // Don't forget that lua_tollsd() can throw Lua errors.
            try
            {
                result.second = lua_tollsd(mState, 1);
            }
            catch (const std::exception& error)
            {
                LL_WARNS("Lua") << desc << " error converting result: " << error.what() << LL_ENDL;
                // lua_tollsd() is designed to be called from a lua_function(),
                // that is, from a C++ function called by Lua. In case of error,
                // it throws a Lua error to be caught by the Lua runtime. expr()
                // is a peculiar use case in which our C++ code is calling
                // lua_tollsd() after return from the Lua runtime. We must catch
                // the exception thrown for a Lua error, else it will propagate
                // out to the main coroutine and terminate the viewer -- but since
                // we instead of the Lua runtime catch it, our lua_State retains
                // its internal error status. Any subsequent lua_pcall() calls
                // with this lua_State will report error regardless of whether the
                // chunk runs successfully. Get a new lua_State().
                initLuaState();
                return { -1, stringize(LLError::Log::classname(error), ": ", error.what()) };
            }
        }
        else
        {
            // multiple entries on the stack
            int index;
            try
            {
                for (index = 1; index <= result.first; ++index)
                {
                    result.second.append(lua_tollsd(mState, index));
                }
            }
            catch (const std::exception& error)
            {
                LL_WARNS("Lua") << desc << " error converting result " << index << ": "
                                << error.what() << LL_ENDL;
                // see above comments regarding lua_State's error status
                initLuaState();
                return { -1, stringize(LLError::Log::classname(error), ": ", error.what()) };
            }
        }
    }
    // pop everything
    lua_settop(mState, 0);

    // If we ran a script that loaded the fiber module, finish up with a call
    // to fiber.run(). That allows a script to kick off some number of fibers,
    // do some work on the main thread and then fall off the end of the script
    // without explicitly appending a call to fiber.run(). run() ensures the
    // rest of the fibers run to completion (or error).
    luaL_checkstack(mState, 4, nullptr);
    // Push _MODULES table on stack
    luaL_findtable(mState, LUA_REGISTRYINDEX, "_MODULES", 1);
    int index = lua_gettop(mState);
    bool found = false;
    // Did this chunk already require('fiber')? To find out, we must search
    // the _MODULES table, because our require() implementation uses the
    // pathname of the module file as the key. Push nil key to start.
    lua_pushnil(mState);
    while (lua_next(mState, index) != 0)
    {
        // key is at index -2, value at index -1
        // "While traversing a table, do not call lua_tolstring directly on a
        // key, unless you know that the key is actually a string. Recall that
        // lua_tolstring changes the value at the given index; this confuses
        // the next call to lua_next."
        // https://www.lua.org/manual/5.1/manual.html#lua_next
        if (lua_type(mState, -2) == LUA_TSTRING &&
            fsyspath(lua_tostdstring(mState, -2)).stem() == "fiber")
        {
            found = true;
            break;
        }
        // pop value so key is at top for lua_next()
        lua_pop(mState, 1);
    }
    if (found)
    {
        // okay, index -1 is a table loaded from a file 'fiber.xxx' --
        // does it have a function named 'run'?
        auto run_type{ lua_getfield(mState, -1, "run") };
        if (run_type == LUA_TFUNCTION)
        {
            // there's a fiber.run() function sitting on the top of the stack
            // -- call it with no arguments, discarding anything it returns
            LL_INFOS("Lua") << desc << " p.s. fiber.run()" << LL_ENDL;
            if (! checkLua(desc, lua_pcall(mState, 0, 0, 0)))
            {
                LL_WARNS("Lua") << desc << " p.s. fiber.run() error: " << mError << LL_ENDL;
                return { -1, mError };
            }
            LL_INFOS("Lua") << desc << " p.s. done." << LL_ENDL;
        }
    }
    // pop everything again
    lua_settop(mState, 0);
    return result;
}

LuaListener::ptr_t LuaState::getListener(lua_State* L)
{
    // have to use one more stack slot
    luaL_checkstack(L, 1, nullptr);
    LuaListener::ptr_t listener;
    // Does this lua_State already have a LuaListener stored in the registry?
    auto keytype{ lua_getfield(L, LUA_REGISTRYINDEX, "event.listener") };
    llassert(keytype == LUA_TNIL || keytype == LUA_TNUMBER);
    if (keytype == LUA_TNUMBER)
    {
        // We do already have a LuaListener. Retrieve it.
        int isint;
        listener = LuaListener::getInstance(lua_tointegerx(L, -1, &isint));
        // Nobody should have destroyed this LuaListener instance!
        llassert(isint && listener);
    }
    // pop the int "event.listener" key
    lua_pop(L, 1);
    return listener;
}

LuaListener::ptr_t LuaState::obtainListener(lua_State* L)
{
    auto listener{ getListener(L) };
    if (! listener)
    {
        // have to use one more stack slot
        luaL_checkstack(L, 1, nullptr);
        // instantiate a new LuaListener, binding the L state -- but use a
        // no-op deleter: we do NOT want this ptr_t to manage the lifespan of
        // this new LuaListener!
        listener.reset(new LuaListener(L), [](LuaListener*){});
        // set its key in the field where we'll look for it later
        lua_pushinteger(L, listener->getKey());
        lua_setfield(L, LUA_REGISTRYINDEX, "event.listener");
    }
    return listener;
}

/*****************************************************************************
*   LuaPopper class
*****************************************************************************/
LuaPopper::~LuaPopper()
{
    if (mCount)
    {
        lua_pop(mState, mCount);
    }
}

/*****************************************************************************
*   LuaFunction class
*****************************************************************************/
LuaFunction::LuaFunction(const std::string_view& name, lua_CFunction function,
                         const std::string_view& helptext)
{
    const auto& [registry, lookup] = getState();
    registry.emplace(name, Registry::mapped_type{ function, helptext });
    lookup.emplace(function, name);
}

void LuaFunction::init(lua_State* L)
{
    const auto& [registry, lookup] = getRState();
    luaL_checkstack(L, 2, nullptr);
    // create LL table --
    // it happens that we know exactly how many non-array members we want
    lua_createtable(L, 0, int(narrow(lookup.size())));
    int idx = lua_gettop(L);
    for (const auto& [name, pair]: registry)
    {
        const auto& [funcptr, helptext] = pair;
        // store funcptr in LL table with saved name
        lua_pushcfunction(L, funcptr, name.c_str());
        lua_setfield(L, idx, name.c_str());
    }
    // store LL in new lua_State's globals
    lua_setglobal(L, "LL");
}

lua_CFunction LuaFunction::get(const std::string& key)
{
    // use find() instead of subscripting to avoid creating an entry for
    // unknown key
    const auto& [registry, lookup] = getState();
    auto found{ registry.find(key) };
    return (found == registry.end())? nullptr : found->second.first;
}

std::pair<LuaFunction::Registry&, LuaFunction::Lookup&> LuaFunction::getState()
{
    // use function-local statics to ensure they're initialized
    static Registry registry;
    static Lookup lookup;
    return { registry, lookup };
}

/*****************************************************************************
*   source_path()
*****************************************************************************/
lua_function(source_path, "return the source path of the running Lua script")
{
    luaL_checkstack(L, 1, nullptr);
    lua_pushstdstring(L, lluau::source_path(L).u8string());
    return 1;
}

/*****************************************************************************
*   source_dir()
*****************************************************************************/
lua_function(source_dir, "return the source directory of the running Lua script")
{
    luaL_checkstack(L, 1, nullptr);
    lua_pushstdstring(L, lluau::source_path(L).parent_path().u8string());
    return 1;
}

/*****************************************************************************
*   abspath()
*****************************************************************************/
lua_function(abspath,
             "for given filesystem path relative to running script, return absolute path")
{
    auto path{ lua_tostdstring(L, 1) };
    lua_pop(L, 1);
    lua_pushstdstring(L, (lluau::source_path(L).parent_path() / path).u8string());
    return 1;
}

/*****************************************************************************
*   check_stop()
*****************************************************************************/
lua_function(check_stop, "ensure that a Lua script responds to viewer shutdown")
{
    LLCoros::checkStop();
    return 0;
}

/*****************************************************************************
*   help()
*****************************************************************************/
lua_function(help,
             "help(): list viewer's Lua functions\n"
             "help(function): show help string for specific function")
{
    auto& luapump{ LLEventPumps::instance().obtain("lua output") };
    const auto& [registry, lookup]{ LuaFunction::getRState() };
    if (! lua_gettop(L))
    {
        // no arguments passed: list all lua_functions
        for (const auto& [name, pair] : registry)
        {
            const auto& [fptr, helptext] = pair;
            luapump.post(helptext);
        }
    }
    else
    {
        // arguments passed: list each of the specified lua_functions
        for (int idx = 1, top = lua_gettop(L); idx <= top; ++idx)
        {
            std::string arg{ stringize("<unknown ", lua_typename(L, lua_type(L, idx)), ">") };
            if (lua_type(L, idx) == LUA_TSTRING)
            {
                arg = lua_tostdstring(L, idx);
            }
            else if (lua_type(L, idx) == LUA_TFUNCTION)
            {
                // Caller passed the actual function instead of its string
                // name. A Lua function is an anonymous callable object; it
                // has a name only by assigment. You can't ask Lua for a
                // function's name, which is why our constructor maintains a
                // reverse Lookup map.
                auto function{ lua_tocfunction(L, idx) };
                if (auto found = lookup.find(function); found != lookup.end())
                {
                    // okay, pass found name to lookup below
                    arg = found->second;
                }
            }

            if (auto found = registry.find(arg); found != registry.end())
            {
                luapump.post(found->second.second);
            }
            else
            {
                luapump.post(arg + ": NOT FOUND");
            }
        }
        // pop all arguments
        lua_settop(L, 0);
    }
    return 0;                       // void return
}

/*****************************************************************************
*   leaphelp()
*****************************************************************************/
lua_function(
    leaphelp,
    "leaphelp(): list viewer's LEAP APIs\n"
    "leaphelp(api): show help for specific api string name")
{
    LLSD request;
    int top{ lua_gettop(L) };
    if (top)
    {
        request = llsd::map("op", "getAPI", "api", lua_tostdstring(L, 1));
    }
    else
    {
        request = llsd::map("op", "getAPIs");
    }
    // pop all args
    lua_settop(L, 0);

    auto& outpump{ LLEventPumps::instance().obtain("lua output") };
    auto listener{ LuaState::obtainListener(L) };
    LLEventStream replyPump("leaphelp", true);
    // ask the LuaListener's LeapListener and suspend calling coroutine until reply
    auto reply{ llcoro::postAndSuspend(request, listener->getCommandName(), replyPump, "reply") };
    reply.erase("reqid");

    if (auto error = reply["error"]; error.isString())
    {
        outpump.post(error.asString());
        return 0;
    }

    if (top)
    {
        // caller wants a specific API
        outpump.post(stringize(reply["name"].asString(), ":\n", reply["desc"].asString()));
        for (const auto& opmap : llsd::inArray(reply["ops"]))
        {
            std::ostringstream reqstr;
            auto req{ opmap["required"] };
            if (req.isArray())
            {
                const char* sep = " (requires ";
                for (const auto& [reqkey, reqval] : llsd::inMap(req))
                {
                    reqstr << sep << reqkey;
                    sep = ", ";
                }
                reqstr << ")";
            }
            outpump.post(stringize("---- ", reply["key"].asString(), " == '",
                                   opmap["name"].asString(), "'", reqstr.str(), ":\n",
                                   opmap["desc"].asString()));
        }
    }
    else
    {
        // caller wants a list of APIs
        for (const auto& [name, data] : llsd::inMap(reply))
        {
            outpump.post(stringize("==== ", name, ":\n", data["desc"].asString()));
        }
    }
    return 0;                       // void return
}

/*****************************************************************************
*   lua_what
*****************************************************************************/
std::ostream& operator<<(std::ostream& out, const lua_what& self)
{
    switch (lua_type(self.L, self.index))
    {
    case LUA_TNONE:
        // distinguish acceptable but non-valid index
        out << "none";
        break;

    case LUA_TNIL:
        out << "nil";
        break;

    case LUA_TBOOLEAN:
    {
        auto oldflags { out.flags() };
        out << std::boolalpha << lua_toboolean(self.L, self.index);
        out.flags(oldflags);
        break;
    }

    case LUA_TNUMBER:
        out << lua_tonumber(self.L, self.index);
        break;

    case LUA_TSTRING:
        out << std::quoted(lua_tostdstring(self.L, self.index));
        break;

    case LUA_TUSERDATA:
    {
        const S32 maxlen = 20;
        S32 binlen{ lua_rawlen(self.L, self.index) };
        LLSD::Binary binary(std::min(maxlen, binlen));
        std::memcpy(binary.data(), lua_touserdata(self.L, self.index), binary.size());
        out << LL::hexdump(binary);
        if (binlen > maxlen)
        {
            out << "...(" << (binlen - maxlen) << " more)";
        }
        break;
    }

    case LUA_TLIGHTUSERDATA:
        out << lua_touserdata(self.L, self.index);
        break;

    default:
        // anything else, don't bother trying to report value, just type
        out << lua_typename(self.L, lua_type(self.L, self.index));
        break;
    }
    return out;
}

/*****************************************************************************
*   lua_stack
*****************************************************************************/
std::ostream& operator<<(std::ostream& out, const lua_stack& self)
{
    out << "stack: [";
    const char* sep = "";
    for (int index = 1; index <= lua_gettop(self.L); ++index)
    {
        out << sep << lua_what(self.L, index);
        sep = ", ";
    }
    out << ']';
    return out;
}
