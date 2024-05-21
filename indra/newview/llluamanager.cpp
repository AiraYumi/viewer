/** 
 * @file llluamanager.cpp
 * @brief classes and functions for interfacing with LUA. 
 *
 * $LicenseInfo:firstyear=2023&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2023, Linden Research, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 *
 */

#include "llviewerprecompiledheaders.h"
#include "llluamanager.h"

#include "fsyspath.h"
#include "llcoros.h"
#include "llerror.h"
#include "lleventcoro.h"
#include "llviewercontrol.h"
#include "lua_function.h"
#include "lualistener.h"
#include "stringize.h"

#include <boost/algorithm/string/replace.hpp>

#include "luau/luacode.h"
#include "luau/lua.h"
#include "luau/luaconf.h"
#include "luau/lualib.h"

#include <sstream>
#include <string_view>
#include <vector>

std::map<std::string, std::string> LLLUAmanager::sScriptNames;

lua_function(sleep, "sleep(seconds): pause the running coroutine")
{
    F32 seconds = lua_tonumber(L, -1);
    lua_pop(L, 1);
    llcoro::suspendUntilTimeout(seconds);
    lluau::set_interrupts_counter(L, 0);
    return 0;
};

// This function consumes ALL Lua stack arguments and returns concatenated
// message string
std::string lua_print_msg(lua_State* L, const std::string_view& level)
{
    // On top of existing Lua arguments, we're going to push tostring() and
    // duplicate each existing stack entry so we can stringize each one.
    luaL_checkstack(L, 2, nullptr);
    luaL_where(L, 1);
    // start with the 'where' info at the top of the stack
    std::ostringstream out;
    out << lua_tostring(L, -1);
    lua_pop(L, 1);
    const char* sep = "";           // 'where' info ends with ": "
    // now iterate over arbitrary args, calling Lua tostring() on each and
    // concatenating with separators
    for (int p = 1, top = lua_gettop(L); p <= top; ++p)
    {
        out << sep;
        sep = " ";
        // push Lua tostring() function -- note, semantically different from
        // lua_tostring()!
        lua_getglobal(L, "tostring");
        // Now the stack is arguments 1 .. N, plus tostring().
        // Push a copy of the argument at index p.
        lua_pushvalue(L, p);
        // pop tostring() and arg-p, pushing tostring(arg-p)
        // (ignore potential error code from lua_pcall() because, if there was
        // an error, we expect the stack top to be an error message -- which
        // we'll print)
        lua_pcall(L, 1, 1, 0);
        out << lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    // pop everything
    lua_settop(L, 0);
    // capture message string
    std::string msg{ out.str() };
    // put message out there for any interested party (*koff* LLFloaterLUADebug *koff*)
    LLEventPumps::instance().obtain("lua output").post(stringize(level, ": ", msg));

    llcoro::suspend();
    return msg;
}

lua_function(print_debug, "print_debug(args...): DEBUG level logging")
{
    LL_DEBUGS("Lua") << lua_print_msg(L, "DEBUG") << LL_ENDL;
    return 0;
}

// also used for print(); see LuaState constructor
lua_function(print_info, "print_info(args...): INFO level logging")
{
    LL_INFOS("Lua") << lua_print_msg(L, "INFO") << LL_ENDL;
    return 0;
}

lua_function(print_warning, "print_warning(args...): WARNING level logging")
{
    LL_WARNS("Lua") << lua_print_msg(L, "WARN") << LL_ENDL;
    return 0;
}

lua_function(post_on, "post_on(pumpname, data): post specified data to specified LLEventPump")
{
    std::string pumpname{ lua_tostdstring(L, 1) };
    LLSD data{ lua_tollsd(L, 2) };
    lua_pop(L, 2);
    LL_DEBUGS("Lua") << "post_on('" << pumpname << "', " << data << ")" << LL_ENDL;
    LLEventPumps::instance().obtain(pumpname).post(data);
    return 0;
}

lua_function(get_event_pumps,
             "get_event_pumps():\n"
             "Returns replypump, commandpump: names of LLEventPumps specific to this chunk.\n"
             "Events posted to replypump are queued for get_event_next().\n"
             "post_on(commandpump, ...) to engage LLEventAPI operations (see helpleap()).")
{
    luaL_checkstack(L, 2, nullptr);
    auto listener{ LuaState::obtainListener(L) };
    // return the reply pump name and the command pump name on caller's lua_State
    lua_pushstdstring(L, listener->getReplyName());
    lua_pushstdstring(L, listener->getCommandName());
    return 2;
}

lua_function(get_event_next,
             "get_event_next():\n"
             "Returns the next (pumpname, data) pair from the replypump whose name\n"
             "is returned by get_event_pumps(). Blocks the calling chunk until an\n"
             "event becomes available.")
{
    luaL_checkstack(L, 2, nullptr);
    auto listener{ LuaState::obtainListener(L) };
    const auto& [pump, data]{ listener->getNext() };
    lua_pushstdstring(L, pump);
    lua_pushllsd(L, data);
    lluau::set_interrupts_counter(L, 0);
    return 2;
}

LLCoros::Future<std::pair<int, LLSD>>
LLLUAmanager::startScriptFile(const std::string& filename)
{
    // Despite returning from startScriptFile(), we need this Promise to
    // remain alive until the callback has fired.
    auto promise{ std::make_shared<LLCoros::Promise<std::pair<int, LLSD>>>() };
    runScriptFile(filename,
                  [promise](int count, LLSD result)
                  { promise->set_value({ count, result }); });
    return LLCoros::getFuture(*promise);
}

std::pair<int, LLSD> LLLUAmanager::waitScriptFile(const std::string& filename)
{
    return startScriptFile(filename).get();
}

void LLLUAmanager::runScriptFile(const std::string &filename, script_result_fn result_cb, script_finished_fn finished_cb)
{
    // A script_result_fn will be called when LuaState::expr() completes.
    LLCoros::instance().launch(filename, [filename, result_cb, finished_cb]()
    {
        ScriptObserver observer(LLCoros::getName(), filename);
        llifstream in_file;
        in_file.open(filename.c_str());

        if (in_file.is_open()) 
        {
            // A script_finished_fn is used to initialize the LuaState.
            // It will be called when the LuaState is destroyed.
            LuaState L(finished_cb);
            std::string text{std::istreambuf_iterator<char>(in_file), {}};
            auto [count, result] = L.expr(filename, text);
            if (result_cb)
            {
                result_cb(count, result);
            }
        }
        else
        {
            auto msg{ stringize("unable to open script file '", filename, "'") };
            LL_WARNS("Lua") << msg << LL_ENDL;
            if (result_cb)
            {
                result_cb(-1, msg);
            }
        }
    });
}

void LLLUAmanager::runScriptLine(const std::string& chunk, script_finished_fn cb)
{
    // A script_finished_fn is used to initialize the LuaState.
    // It will be called when the LuaState is destroyed.
    LuaState L(cb);
    runScriptLine(L, chunk);
}

void LLLUAmanager::runScriptLine(const std::string& chunk, script_result_fn cb)
{
    LuaState L;
    // A script_result_fn will be called when LuaState::expr() completes.
    runScriptLine(L, chunk, cb);
}

LLCoros::Future<std::pair<int, LLSD>>
LLLUAmanager::startScriptLine(LuaState& L, const std::string& chunk)
{
    // Despite returning from startScriptLine(), we need this Promise to
    // remain alive until the callback has fired.
    auto promise{ std::make_shared<LLCoros::Promise<std::pair<int, LLSD>>>() };
    runScriptLine(L, chunk,
                  [promise](int count, LLSD result)
                  { promise->set_value({ count, result }); });
    return LLCoros::getFuture(*promise);
}

std::pair<int, LLSD> LLLUAmanager::waitScriptLine(LuaState& L, const std::string& chunk)
{
    return startScriptLine(L, chunk).get();
}

void LLLUAmanager::runScriptLine(LuaState& L, const std::string& chunk, script_result_fn cb)
{
    // find a suitable abbreviation for the chunk string
    std::string shortchunk{ chunk };
    const size_t shortlen = 40;
    std::string::size_type eol = shortchunk.find_first_of("\r\n");
    if (eol != std::string::npos)
        shortchunk = shortchunk.substr(0, eol);
    if (shortchunk.length() > shortlen)
        shortchunk = stringize(shortchunk.substr(0, shortlen), "...");

    std::string desc{ stringize("lua: ", shortchunk) };
    LLCoros::instance().launch(desc, [&L, desc, chunk, cb]()
    {
        auto [count, result] = L.expr(desc, chunk);
        if (cb)
        {
            cb(count, result);
        }
    });
}

void LLLUAmanager::runScriptOnLogin()
{
#ifndef LL_TEST
    std::string filename = gSavedSettings.getString("AutorunLuaScriptName");
    if (filename.empty()) 
    {
        LL_INFOS() << "Script name wasn't set." << LL_ENDL;
        return;
    }

    filename = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, filename);
    if (!gDirUtilp->fileExists(filename)) 
    {
        LL_INFOS() << filename << " was not found." << LL_ENDL;
        return;
    }

    runScriptFile(filename);
#endif // ! LL_TEST
}

std::string read_file(const std::string &name)
{
    llifstream in_file;
    in_file.open(name.c_str());

    if (in_file.is_open())
    {
        return std::string{std::istreambuf_iterator<char>(in_file), {}};
    }

    return {};
}

lua_function(require, "require(module_name) : load module_name.lua from known places")
{
    std::string name = lua_tostdstring(L, 1);
    lua_pop(L, 1);

    // resolveRequire() does not return in case of error.
    LLRequireResolver::resolveRequire(L, name);

    // resolveRequire() returned the newly-loaded module on the stack top.
    // Return it.
    return 1;
}

// push loaded module or throw Lua error
void LLRequireResolver::resolveRequire(lua_State *L, std::string path)
{
    LLRequireResolver resolver(L, std::move(path));
    // findModule() pushes the loaded module or throws a Lua error.
    resolver.findModule();
}

LLRequireResolver::LLRequireResolver(lua_State *L, const std::string& path) :
    mPathToResolve(fsyspath(path).lexically_normal()),
    L(L)
{
    mSourceDir = lluau::source_path(L).parent_path();

    if (mPathToResolve.is_absolute())
        luaL_argerrorL(L, 1, "cannot require a full path");
}

/**
 * Remove a particular stack index on exit from enclosing scope.
 * If you pass a negative index (meaning relative to the current stack top),
 * converts to an absolute index. The point of LuaRemover is to remove the
 * entry at the specified index regardless of subsequent pushes to the stack.
 */
class LuaRemover
{
public:
    LuaRemover(lua_State* L, int index):
        mState(L),
        mIndex(lua_absindex(L, index))
    {}
    LuaRemover(const LuaRemover&) = delete;
    LuaRemover& operator=(const LuaRemover&) = delete;
    ~LuaRemover()
    {
        lua_remove(mState, mIndex);
    }

private:
    lua_State* mState;
    int mIndex;
};

// push the loaded module or throw a Lua error
void LLRequireResolver::findModule()
{
    // If mPathToResolve is absolute, this replaces mSourceDir.
    auto absolutePath = (mSourceDir / mPathToResolve).u8string();

    // Push _MODULES table on stack for checking and saving to the cache
    luaL_findtable(L, LUA_REGISTRYINDEX, "_MODULES", 1);
    // Remove that stack entry no matter how we exit
    LuaRemover rm_MODULES(L, -1);

    // Check if the module is already in _MODULES table, read from file
    // otherwise.
    // findModuleImpl() pushes module if found, nothing if not, may throw Lua
    // error.
    if (findModuleImpl(absolutePath))
        return;

    // not already cached - prep error message just in case
    auto fail{
        [L=L, path=mPathToResolve.u8string()]()
        { luaL_error(L, "could not find require('%s')", path.data()); }};

    if (mPathToResolve.is_absolute())
    {
        // no point searching known directories for an absolute path
        fail();
    }

    std::vector<fsyspath> lib_paths
    {
        gDirUtilp->getExpandedFilename(LL_PATH_SCRIPTS, "lua"),
#ifdef LL_TEST
        // Build-time tests don't have the app bundle - use source tree.
        fsyspath(__FILE__).parent_path() / "scripts" / "lua",
#endif
    };

    for (const auto& path : lib_paths)
    {
        std::string absolutePathOpt = (path / mPathToResolve).u8string();

        if (absolutePathOpt.empty())
            luaL_error(L, "error requiring module '%s'", mPathToResolve.u8string().data());

        if (findModuleImpl(absolutePathOpt))
            return;
    }

    // not found
    fail();
}

// expects _MODULES table on stack top (and leaves it there)
// - if found, pushes loaded module and returns true
// - not found, pushes nothing and returns false
// - may throw Lua error
bool LLRequireResolver::findModuleImpl(const std::string& absolutePath)
{
    std::string possibleSuffixedPaths[] = {absolutePath + ".luau", absolutePath + ".lua"};

    for (const auto& suffixedPath : possibleSuffixedPaths)
    {
         // Check _MODULES cache for module
        lua_getfield(L, -1, suffixedPath.data());
        if (!lua_isnil(L, -1))
        {
            return true;
        }
        lua_pop(L, 1);

        // Try to read the matching file
        std::string source = read_file(suffixedPath);
        if (!source.empty())
        {
            // Try to run the loaded source. This will leave either a string
            // error message or the module contents on the stack top.
            runModule(suffixedPath, source);

            // If the stack top is an error message string, raise it.
            if (lua_isstring(L, -1))
                lua_error(L);

            // duplicate the new module: _MODULES newmodule newmodule
            lua_pushvalue(L, -1);
            // store _MODULES[found path] = newmodule
            lua_setfield(L, -3, suffixedPath.data());

            return true;
        }
    }

    return false;
}

// push string error message or new module
void LLRequireResolver::runModule(const std::string& desc, const std::string& code)
{
    // Here we just loaded a new module 'code', need to run it and get its result.
    // Module needs to run in a new thread, isolated from the rest.
    // Note: we create ML on main thread so that it doesn't inherit environment of L.
    lua_State *GL = lua_mainthread(L);
//  lua_State *ML = lua_newthread(GL);
    // Try loading modules on Lua's main thread instead.
    lua_State *ML = GL;
    // lua_newthread() pushed the new thread object on GL's stack. Move to L's.
//  lua_xmove(GL, L, 1);

    // new thread needs to have the globals sandboxed
//  luaL_sandboxthread(ML);

    {
        // If loadstring() returns (! LUA_OK) then there's an error message on
        // the stack. If it returns LUA_OK then the newly-loaded module code
        // is on the stack.
        if (lluau::loadstring(ML, desc, code) == LUA_OK)
        {
            // luau uses Lua 5.3's version of lua_resume():
            // run the coroutine on ML, "from" L, passing no arguments.
//          int status = lua_resume(ML, L, 0);
            // we expect one return value
            int status = lua_pcall(ML, 0, 1, 0);

            if (status == LUA_OK)
            {
                if (lua_gettop(ML) == 0)
                    lua_pushfstring(ML, "module %s must return a value", desc.data());
                else if (!lua_istable(ML, -1) && !lua_isfunction(ML, -1))
                    lua_pushfstring(ML, "module %s must return a table or function, not %s",
                                    desc.data(), lua_typename(ML, lua_type(ML, -1)));
            }
            else if (status == LUA_YIELD)
            {
                lua_pushfstring(ML, "module %s can not yield", desc.data());
            }
            else if (!lua_isstring(ML, -1))
            {
                lua_pushfstring(ML, "unknown error while running module %s", desc.data());
            }
        }
    }
    // There's now a return value (string error message or module) on top of ML.
    // Move return value to L's stack.
    if (ML != L)
    {
        lua_xmove(ML, L, 1);
    }
    // remove ML from L's stack
//  lua_remove(L, -2);
//  // DON'T call lua_close(ML)! Since ML is only a thread of L, corrupts L too!    
//  lua_close(ML);
}
