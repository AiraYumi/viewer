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

#include "llerror.h"
#include "lleventcoro.h"
#include "lua_function.h"
#include "lualistener.h"
#include "stringize.h"

// skip all these link dependencies for integration testing
#ifndef LL_TEST
#include "lluilistener.h"
#include "llviewercontrol.h"

// FIXME extremely hacky way to get to the UI Listener framework. There's
// a cleaner way.
extern LLUIListener sUIListener;
#endif // ! LL_TEST

#include <boost/algorithm/string/replace.hpp>

#include "luau/luacode.h"
#include "luau/lua.h"
#include "luau/luaconf.h"
#include "luau/lualib.h"

#include <sstream>
#include <string_view>
#include <vector>

/*
// This function consumes ALL Lua stack arguments and returns concatenated
// message string
std::string lua_print_msg_args(lua_State* L, const std::string_view& level)
{
    // On top of existing Lua arguments, push 'where' info
    luaL_checkstack(L, 1, nullptr);
    luaL_where(L, 1);
    // start with the 'where' info at the top of the stack
    std::ostringstream out;
    out << lua_tostring(L, -1);
    lua_pop(L, 1);
    const char* sep = "";           // 'where' info ends with ": "
    // now iterate over arbitrary args, calling Lua tostring() on each and
    // concatenating with separators
    for (int p = 1; p <= lua_gettop(L); ++p)
    {
        out << sep;
        sep = " ";
        // push Lua tostring() function -- note, semantically different from
        // lua_tostring()!
        lua_getglobal(L, "tostring");
        // Now the stack is arguments 1 .. N, plus tostring().
        // Rotate downwards, producing stack args 2 .. N, tostring(), arg1.
        lua_rotate(L, 1, -1);
        // pop tostring() and arg1, pushing tostring(arg1)
        // (ignore potential error code from lua_pcall() because, if there was
        // an error, we expect the stack top to be an error message -- which
        // we'll print)
        lua_pcall(L, 1, 1, 0);
        // stack now holds args 2 .. N, tostring(arg1)
        out << lua_tostring(L, -1);
    }
    // pop everything
    lua_settop(L, 0);
    // capture message string
    std::string msg{ out.str() };
    // put message out there for any interested party (*koff* LLFloaterLUADebug *koff*)
    LLEventPumps::instance().obtain("lua output").post(stringize(level, ": ", msg));
    return msg;
}
*/

std::string lua_print_msg(lua_State *L, const std::string_view &level)
{
    lua_getglobal(L, "tostring");
    
    lua_pushvalue(L, -1); /* function to be called */
    lua_pushvalue(L, 1);  /* value to print */
    lua_call(L, 1, 1);
    std::string msg = lua_tostring(L, -1);

    LLEventPumps::instance().obtain("lua output").post(stringize(level, ": ", msg));
    return msg;
}

lua_function(print_debug)
{
    LL_DEBUGS("Lua") << lua_print_msg(L, "DEBUG") << LL_ENDL;
    return 0;
}

// also used for print(); see LuaState constructor
lua_function(print_info)
{
    LL_INFOS("Lua") << lua_print_msg(L, "INFO") << LL_ENDL;
    return 0;
}

lua_function(print_warning)
{
    LL_WARNS("Lua") << lua_print_msg(L, "WARN") << LL_ENDL;
    return 0;
}

#ifndef LL_TEST

lua_function(run_ui_command)
{
	int top = lua_gettop(L);
	std::string func_name;
	if (top >= 1)
	{
		func_name = lua_tostring(L,1);
	}
	std::string parameter;
	if (top >= 2)
	{
		parameter = lua_tostring(L,2);
	}
	LL_WARNS("LUA") << "running ui func " << func_name << " parameter " << parameter << LL_ENDL;
	LLSD event;
	event["function"] = func_name;
	if (!parameter.empty())
	{
		event["parameter"] = parameter; 
	}
	sUIListener.call(event);

	lua_settop(L, 0);
	return 0;
}
#endif // ! LL_TEST

lua_function(post_on)
{
    std::string pumpname{ lua_tostdstring(L, 1) };
    LLSD data{ lua_tollsd(L, 2) };
    lua_pop(L, 2);
    LL_INFOS("Lua") << "post_on('" << pumpname << "', " << data << ")" << LL_ENDL;
    LLEventPumps::instance().obtain(pumpname).post(data);
    return 0;
}

lua_function(listen_events)
{
    if (! lua_isfunction(L, 1))
    {
        luaL_typeerror(L, 1, "function");
        return 0;
    }
    luaL_checkstack(L, 2, nullptr);

    // Get the lua_State* for the main thread of this state, in case we were
    // called from a coroutine thread. We're going to make callbacks into Lua
    // code, and we want to do it on the main thread rather than a (possibly
    // suspended) coroutine thread.
    // Registry table is at pseudo-index LUA_REGISTRYINDEX
    // Main thread is at registry key LUA_RIDX_MAINTHREAD
    auto regtype {lua_rawgeti(L, LUA_REGISTRYINDEX, 1 /*LUA_RIDX_MAINTHREAD*/)};
    // Not finding the main thread at the documented place isn't a user error,
    // it's a Problem
    llassert_always(regtype == LUA_TTHREAD);
    lua_State* mainthread{ lua_tothread(L, -1) };
    // pop the main thread
    lua_pop(L, 1);

    luaL_checkstack(mainthread, 1, nullptr);
    LuaListener::ptr_t listener;
    // Does the main thread already have a LuaListener stored in the registry?
    // That is, has this Lua chunk already called listen_events()?
    auto keytype{ lua_getfield(mainthread, LUA_REGISTRYINDEX, "event.listener") };
    llassert(keytype == LUA_TNIL || keytype == LUA_TNUMBER);
    if (keytype == LUA_TNUMBER)
    {
        // We do already have a LuaListener. Retrieve it.
        int isint;
        listener = LuaListener::getInstance(lua_tointegerx(mainthread, -1, &isint));
        // pop the int "event.listener" key
        lua_pop(mainthread, 1);
        // Nobody should have destroyed this LuaListener instance!
        llassert(isint && listener);
    }
    else
    {
        // pop the nil "event.listener" key
        lua_pop(mainthread, 1);
        // instantiate a new LuaListener, binding the mainthread state -- but
        // use a no-op deleter: we do NOT want to delete this new LuaListener
        // on return from listen_events()!
        listener.reset(new LuaListener(mainthread), [](LuaListener*){});
        // set its key in the field where we'll look for it later
        lua_pushinteger(mainthread, listener->getKey());
        lua_setfield(mainthread, LUA_REGISTRYINDEX, "event.listener");
    }

    // Now that we've found or created our LuaListener, store the passed Lua
    // function as the callback. Beware: our caller passed the function on L's
    // stack, but we want to store it on the mainthread registry.
    if (L != mainthread)
    {
        // push 1 value (the Lua function) from L's stack to mainthread's
        lua_xmove(L, mainthread, 1);
    }
    lua_setfield(mainthread, LUA_REGISTRYINDEX, "event.function");

    // return the reply pump name and the command pump name on caller's lua_State
    lua_pushstdstring(L, listener->getReplyName());
    lua_pushstdstring(L, listener->getCommandName());
    return 2;
}

lua_function(await_event)
{
    // await_event(pumpname [, timeout [, value to return if timeout (default nil)]])
    auto pumpname{ lua_tostdstring(L, 1) };
    LLSD result;
    if (lua_gettop(L) > 1)
    {
        auto timeout{ lua_tonumber(L, 2) };
        // with no 3rd argument, should be LLSD()
        auto dftval{ lua_tollsd(L, 3) };
        lua_settop(L, 0);
        result = llcoro::suspendUntilEventOnWithTimeout(pumpname, timeout, dftval);
    }
    else
    {
        // no timeout
        lua_pop(L, 1);
        result = llcoro::suspendUntilEventOn(pumpname);
    }
    lua_pushllsd(L, result);
    return 1;
}

void LLLUAmanager::runScriptFile(const std::string& filename, script_finished_fn cb)
{
    std::string desc{ stringize("runScriptFile('", filename, "')") };
    LLCoros::instance().launch(desc, [desc, filename, cb]()
    {
        LuaState L(desc, cb);

        auto LUA_sleep_func = [](lua_State *L)
        {
            F32 seconds = lua_tonumber(L, -1);
            lua_pop(L, 1);
            llcoro::suspendUntilTimeout(seconds);
            return 0;
        };

        lua_register(L, "sleep", LUA_sleep_func);

        llifstream in_file;
        in_file.open(filename.c_str());

        if (in_file.is_open()) 
        {
            std::string text{std::istreambuf_iterator<char>(in_file),
                             std::istreambuf_iterator<char>()};
            L.checkLua(lluau::dostring(L, desc, text));
        }
        else
        {
            LL_WARNS("Lua") << "unable to open script file '" << filename << "'" << LL_ENDL;
        }
    });
}

void LLLUAmanager::runScriptLine(const std::string& cmd, script_finished_fn cb)
{
    // find a suitable abbreviation for the cmd string
    std::string_view shortcmd{ cmd };
    const size_t shortlen = 40;
    std::string::size_type eol = shortcmd.find_first_of("\r\n");
    if (eol != std::string::npos)
        shortcmd = shortcmd.substr(0, eol);
    if (shortcmd.length() > shortlen)
        shortcmd = stringize(shortcmd.substr(0, shortlen), "...");

    std::string desc{ stringize("runScriptLine('", shortcmd, "')") };
    LLCoros::instance().launch(desc, [desc, cmd, cb]()
    {
        LuaState L(desc, cb);
        L.checkLua(lluau::dostring(L, desc, cmd));
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
