/**
 * @file lleventtimer.cpp
 * @brief Cross-platform objects for doing timing
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 */

#include "linden_common.h"
#include "lleventtimer.h"

//////////////////////////////////////////////////////////////////////////////
//
//      LLEventTimer Implementation
//
//////////////////////////////////////////////////////////////////////////////

LLEventTimer::LLEventTimer(F32 period):
    mPeriod(period)
{
    start();
}

LLEventTimer::LLEventTimer(const LLDate& time):
    LLEventTimer(F32(time.secondsSinceEpoch() - LLDate::now().secondsSinceEpoch()))
{}

LLEventTimer::~LLEventTimer()
{
}

void LLEventTimer::start()
{
    mTimer = LL::Timers::instance().scheduleEvery([this]{ return tick(); }, mPeriod);
}

void LLEventTimer::stop()
{
    LL::Timers::instance().cancel(mTimer);
}

bool LLEventTimer::isRunning()
{
    return LL::Timers::instance().isRunning(mTimer);
}

F32 LLEventTimer::getRemaining()
{
    return LL::Timers::instance().timeUntilCall(mTimer);
}
