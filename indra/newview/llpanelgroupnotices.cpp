/** 
 * @file llpanelgroupnotices.cpp
 * @brief A panel to display group notices.
 *
 * $LicenseInfo:firstyear=2006&license=viewergpl$
 * 
 * Copyright (c) 2006-2009, Linden Research, Inc.
 * 
 * Second Life Viewer Source Code
 * The source code in this file ("Source Code") is provided by Linden Lab
 * to you under the terms of the GNU General Public License, version 2.0
 * ("GPL"), unless you have obtained a separate licensing agreement
 * ("Other License"), formally executed by you and Linden Lab.  Terms of
 * the GPL can be found in doc/GPL-license.txt in this distribution, or
 * online at http://secondlifegrid.net/programs/open_source/licensing/gplv2
 * 
 * There are special exceptions to the terms and conditions of the GPL as
 * it is applied to this Source Code. View the full text of the exception
 * in the file doc/FLOSS-exception.txt in this software distribution, or
 * online at
 * http://secondlifegrid.net/programs/open_source/licensing/flossexception
 * 
 * By copying, modifying or distributing this software, you acknowledge
 * that you have read and understood your obligations described above,
 * and agree to abide by those obligations.
 * 
 * ALL LINDEN LAB SOURCE CODE IS PROVIDED "AS IS." LINDEN LAB MAKES NO
 * WARRANTIES, EXPRESS, IMPLIED OR OTHERWISE, REGARDING ITS ACCURACY,
 * COMPLETENESS OR PERFORMANCE.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llpanelgroupnotices.h"

#include "llview.h"

#include "llcachename.h"
#include "llinventory.h"
#include "llviewerinventory.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llfloaterinventory.h"
#include "llagent.h"
#include "llagentui.h"
#include "lltooldraganddrop.h"

#include "lllineeditor.h"
#include "lltexteditor.h"
#include "llbutton.h"
#include "lliconctrl.h"
#include "llcheckboxctrl.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lltextbox.h"
#include "lltrans.h"

#include "roles_constants.h"
#include "llviewerwindow.h"
#include "llviewermessage.h"
#include "llnotificationsutil.h"

static LLRegisterPanelClassWrapper<LLPanelGroupNotices> t_panel_group_notices("panel_group_notices");


/////////////////////////
// LLPanelGroupNotices //
/////////////////////////
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// Class LLDropTarget
//
// This handy class is a simple way to drop something on another
// view. It handles drop events, always setting itself to the size of
// its parent.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
class LLGroupDropTarget : public LLView
{
public:
	struct Params : public LLInitParam::Block<Params, LLView::Params>
	{
		// *NOTE: These parameters logically Mandatory, but are not
		// specified in XML files, hence Optional
		Optional<LLPanelGroupNotices*> panel;
		Optional<LLUUID>	group_id;
		Params()
		:	panel("panel"),
			group_id("group_id")
		{
			mouse_opaque(false);
			follows.flags(FOLLOWS_ALL);
		}
	};
	LLGroupDropTarget(const Params&);
	~LLGroupDropTarget() {};

	void doDrop(EDragAndDropType cargo_type, void* cargo_data);

	//
	// LLView functionality
	virtual BOOL handleDragAndDrop(S32 x, S32 y, MASK mask, BOOL drop,
								   EDragAndDropType cargo_type,
								   void* cargo_data,
								   EAcceptance* accept,
								   std::string& tooltip_msg);
	void setPanel (LLPanelGroupNotices* panel) {mGroupNoticesPanel = panel;};
	void setGroup (LLUUID group) {mGroupID = group;};

protected:
	LLPanelGroupNotices* mGroupNoticesPanel;
	LLUUID	mGroupID;
};

static LLDefaultChildRegistry::Register<LLGroupDropTarget> r("group_drop_target");

LLGroupDropTarget::LLGroupDropTarget(const LLGroupDropTarget::Params& p) 
:	LLView(p),
	mGroupNoticesPanel(p.panel),
	mGroupID(p.group_id)
{}

void LLGroupDropTarget::doDrop(EDragAndDropType cargo_type, void* cargo_data)
{
	llinfos << "LLGroupDropTarget::doDrop()" << llendl;
}

BOOL LLGroupDropTarget::handleDragAndDrop(S32 x, S32 y, MASK mask, BOOL drop,
									 EDragAndDropType cargo_type,
									 void* cargo_data,
									 EAcceptance* accept,
									 std::string& tooltip_msg)
{
	BOOL handled = FALSE;

	if (!gAgent.hasPowerInGroup(mGroupID,GP_NOTICES_SEND))
	{
		*accept = ACCEPT_NO;
		return TRUE;
	}

	if(getParent())
	{
		// check if inside
		//LLRect parent_rect = mParentView->getRect();
		//getRect().set(0, parent_rect.getHeight(), parent_rect.getWidth(), 0);
		handled = TRUE;

		// check the type
		switch(cargo_type)
		{
		case DAD_TEXTURE:
		case DAD_SOUND:
		case DAD_LANDMARK:
		case DAD_SCRIPT:
		case DAD_OBJECT:
		case DAD_NOTECARD:
		case DAD_CLOTHING:
		case DAD_BODYPART:
		case DAD_ANIMATION:
		case DAD_GESTURE:
		case DAD_CALLINGCARD:
		{
			LLViewerInventoryItem* inv_item = (LLViewerInventoryItem*)cargo_data;
			if(gInventory.getItem(inv_item->getUUID())
				&& LLToolDragAndDrop::isInventoryGroupGiveAcceptable(inv_item))
			{
				// *TODO: get multiple object transfers working
				*accept = ACCEPT_YES_COPY_SINGLE;
				if(drop)
				{
					mGroupNoticesPanel->setItem(inv_item);
				}
			}
			else
			{
				// It's not in the user's inventory (it's probably
				// in an object's contents), so disallow dragging
				// it here.  You can't give something you don't
				// yet have.
				*accept = ACCEPT_NO;
			}
			break;
		}
		case DAD_CATEGORY:
		default:
			*accept = ACCEPT_NO;
			break;
		}
	}
	return handled;
}

//-----------------------------------------------------------------------------
// LLPanelGroupNotices
//-----------------------------------------------------------------------------
std::string build_notice_date(const U32& the_time)
{
	// ISO 8601 date format

	time_t t = (time_t)the_time;
	
	if (!t)
	{
		time(&t);
	}
	
	std::string dateStr = "["+LLTrans::getString("LTimeMthNum")+"]/["
								+LLTrans::getString("LTimeDay")+"]/["
								+LLTrans::getString("LTimeYear")+"]";
	LLSD substitution;
	substitution["datetime"] = (S32) t;
	LLStringUtil::format (dateStr, substitution);
	return dateStr;
}

LLPanelGroupNotices::LLPanelGroupNotices() :
	LLPanelGroupTab(),
	mInventoryItem(NULL),
	mInventoryOffer(NULL)
{
	
	
}

LLPanelGroupNotices::~LLPanelGroupNotices()
{
	sInstances.erase(mGroupID);

	if (mInventoryOffer)
	{
		// Cancel the inventory offer.
		mInventoryOffer->forceResponse(IOR_DECLINE);

		mInventoryOffer = NULL;
	}
}


BOOL LLPanelGroupNotices::isVisibleByAgent(LLAgent* agentp)
{
	return mAllowEdit &&
		agentp->hasPowerInGroup(mGroupID, GP_NOTICES_SEND | GP_NOTICES_RECEIVE);
}

BOOL LLPanelGroupNotices::postBuild()
{
	bool recurse = true;

	mNoticesList = getChild<LLScrollListCtrl>("notice_list",recurse);
	mNoticesList->setCommitOnSelectionChange(TRUE);
	mNoticesList->setCommitCallback(onSelectNotice, this);

	mBtnNewMessage = getChild<LLButton>("create_new_notice",recurse);
	mBtnNewMessage->setClickedCallback(onClickNewMessage, this);
	mBtnNewMessage->setEnabled(gAgent.hasPowerInGroup(mGroupID, GP_NOTICES_SEND));

	mBtnGetPastNotices = getChild<LLButton>("refresh_notices",recurse);
	mBtnGetPastNotices->setClickedCallback(onClickRefreshNotices, this);

	// Create
	mCreateSubject = getChild<LLLineEditor>("create_subject",recurse);
	mCreateMessage = getChild<LLTextEditor>("create_message",recurse);

	mCreateInventoryName =  getChild<LLLineEditor>("create_inventory_name",recurse);
	mCreateInventoryName->setTabStop(FALSE);
	mCreateInventoryName->setEnabled(FALSE);

	mCreateInventoryIcon = getChild<LLIconCtrl>("create_inv_icon",recurse);
	mCreateInventoryIcon->setVisible(FALSE);

	mBtnSendMessage = getChild<LLButton>("send_notice",recurse);
	mBtnSendMessage->setClickedCallback(onClickSendMessage, this);

	mBtnRemoveAttachment = getChild<LLButton>("remove_attachment",recurse);
	mBtnRemoveAttachment->setClickedCallback(onClickRemoveAttachment, this);
	mBtnRemoveAttachment->setEnabled(FALSE);

	// View
	mViewSubject = getChild<LLLineEditor>("view_subject",recurse);
	mViewMessage = getChild<LLTextEditor>("view_message",recurse);

	mViewInventoryName =  getChild<LLLineEditor>("view_inventory_name",recurse);
	mViewInventoryName->setTabStop(FALSE);
	mViewInventoryName->setEnabled(FALSE);

	mViewInventoryIcon = getChild<LLIconCtrl>("view_inv_icon",recurse);
	mViewInventoryIcon->setVisible(FALSE);

	mBtnOpenAttachment = getChild<LLButton>("open_attachment",recurse);
	mBtnOpenAttachment->setClickedCallback(onClickOpenAttachment, this);

	mNoNoticesStr = getString("no_notices_text");

	mPanelCreateNotice = getChild<LLPanel>("panel_create_new_notice",recurse);
	mPanelViewNotice = getChild<LLPanel>("panel_view_past_notice",recurse);

	LLGroupDropTarget* target = getChild<LLGroupDropTarget> ("drop_target");
	target->setPanel (this);
	target->setGroup (mGroupID);

	arrangeNoticeView(VIEW_PAST_NOTICE);

	return LLPanelGroupTab::postBuild();
}

void LLPanelGroupNotices::activate()
{
	if(mNoticesList)
		mNoticesList->deleteAllItems();
	
	BOOL can_send = gAgent.hasPowerInGroup(mGroupID,GP_NOTICES_SEND);
	BOOL can_receive = gAgent.hasPowerInGroup(mGroupID,GP_NOTICES_RECEIVE);

	mPanelViewNotice->setEnabled(can_receive);
	mPanelCreateNotice->setEnabled(can_send);

	// Always disabled to stop direct editing of attachment names
	mCreateInventoryName->setEnabled(FALSE);
	mViewInventoryName->setEnabled(FALSE);

	// If we can receive notices, grab them right away.
	if (can_receive)
	{
		onClickRefreshNotices(this);
	}
}

void LLPanelGroupNotices::setItem(LLPointer<LLInventoryItem> inv_item)
{
	mInventoryItem = inv_item;

	BOOL item_is_multi = FALSE;
	if ( inv_item->getFlags() & LLInventoryItem::II_FLAGS_OBJECT_HAS_MULTIPLE_ITEMS )
	{
		item_is_multi = TRUE;
	};

	std::string icon_name = get_item_icon_name(inv_item->getType(),
										inv_item->getInventoryType(),
										inv_item->getFlags(),
										item_is_multi );

	mCreateInventoryIcon->setValue(icon_name);
	mCreateInventoryIcon->setVisible(TRUE);

	std::stringstream ss;
	ss << "        " << mInventoryItem->getName();

	mCreateInventoryName->setText(ss.str());
	mBtnRemoveAttachment->setEnabled(TRUE);
}

void LLPanelGroupNotices::onClickRemoveAttachment(void* data)
{
	LLPanelGroupNotices* self = (LLPanelGroupNotices*)data;
	self->mInventoryItem = NULL;
	self->mCreateInventoryName->clear();
	self->mCreateInventoryIcon->setVisible(FALSE);
	self->mBtnRemoveAttachment->setEnabled(FALSE);
}

//static 
void LLPanelGroupNotices::onClickOpenAttachment(void* data)
{
	LLPanelGroupNotices* self = (LLPanelGroupNotices*)data;

	self->mInventoryOffer->forceResponse(IOR_ACCEPT);
	self->mInventoryOffer = NULL;
	self->mBtnOpenAttachment->setEnabled(FALSE);
}

void LLPanelGroupNotices::onClickSendMessage(void* data)
{
	LLPanelGroupNotices* self = (LLPanelGroupNotices*)data;
	
	if (self->mCreateSubject->getText().empty())
	{
		// Must supply a subject
		LLNotificationsUtil::add("MustSpecifyGroupNoticeSubject");
		return;
	}
	send_group_notice(
			self->mGroupID,
			self->mCreateSubject->getText(),
			self->mCreateMessage->getText(),
			self->mInventoryItem);


	//instantly add new notice. actual notice will be added after ferreshNotices call
	LLUUID id = LLUUID::generateNewID();
	std::string subj = self->mCreateSubject->getText();
	std::string name ;
	LLAgentUI::buildFullname(name);
	U32 timestamp = 0;

	LLSD row;
	row["id"] = id;
	
	row["columns"][0]["column"] = "icon";

	row["columns"][1]["column"] = "subject";
	row["columns"][1]["value"] = subj;

	row["columns"][2]["column"] = "from";
	row["columns"][2]["value"] = name;

	row["columns"][3]["column"] = "date";
	row["columns"][3]["value"] = build_notice_date(timestamp);

	row["columns"][4]["column"] = "sort";
	row["columns"][4]["value"] = llformat( "%u", timestamp);

	self->mNoticesList->addElement(row, ADD_BOTTOM);

	self->mCreateMessage->clear();
	self->mCreateSubject->clear();
	onClickRemoveAttachment(data);

	self->arrangeNoticeView(VIEW_PAST_NOTICE);
}

//static 
void LLPanelGroupNotices::onClickNewMessage(void* data)
{
	LLPanelGroupNotices* self = (LLPanelGroupNotices*)data;

	self->arrangeNoticeView(CREATE_NEW_NOTICE);

	if (self->mInventoryOffer)
	{
		self->mInventoryOffer->forceResponse(IOR_DECLINE);
		self->mInventoryOffer = NULL;
	}

	self->mCreateSubject->clear();
	self->mCreateMessage->clear();
	if (self->mInventoryItem) onClickRemoveAttachment(self);
	self->mNoticesList->deselectAllItems(TRUE); // TRUE == don't commit on chnage
}

void LLPanelGroupNotices::refreshNotices()
{
	onClickRefreshNotices(this);
	/*
	lldebugs << "LLPanelGroupNotices::onClickGetPastNotices" << llendl;
	
	mNoticesList->deleteAllItems();

	LLMessageSystem* msg = gMessageSystem;
	msg->newMessage("GroupNoticesListRequest");
	msg->nextBlock("AgentData");
	msg->addUUID("AgentID",gAgent.getID());
	msg->addUUID("SessionID",gAgent.getSessionID());
	msg->nextBlock("Data");
	msg->addUUID("GroupID",self->mGroupID);
	gAgent.sendReliableMessage();
	*/
	
}

void LLPanelGroupNotices::onClickRefreshNotices(void* data)
{
	lldebugs << "LLPanelGroupNotices::onClickGetPastNotices" << llendl;
	LLPanelGroupNotices* self = (LLPanelGroupNotices*)data;
	
	self->mNoticesList->deleteAllItems();

	LLMessageSystem* msg = gMessageSystem;
	msg->newMessage("GroupNoticesListRequest");
	msg->nextBlock("AgentData");
	msg->addUUID("AgentID",gAgent.getID());
	msg->addUUID("SessionID",gAgent.getSessionID());
	msg->nextBlock("Data");
	msg->addUUID("GroupID",self->mGroupID);
	gAgent.sendReliableMessage();
}

//static
std::map<LLUUID,LLPanelGroupNotices*> LLPanelGroupNotices::sInstances;

// static
void LLPanelGroupNotices::processGroupNoticesListReply(LLMessageSystem* msg, void** data)
{
	LLUUID group_id;
	msg->getUUID("AgentData", "GroupID", group_id);

	std::map<LLUUID,LLPanelGroupNotices*>::iterator it = sInstances.find(group_id);
	if (it == sInstances.end())
	{
		llinfos << "Group Panel Notices " << group_id << " no longer in existence."
				<< llendl;
		return;
	}
	
	LLPanelGroupNotices* selfp = it->second;
	if(!selfp)
	{
		llinfos << "Group Panel Notices " << group_id << " no longer in existence."
				<< llendl;
		return;
	}

	selfp->processNotices(msg);
}

void LLPanelGroupNotices::processNotices(LLMessageSystem* msg)
{
	LLUUID id;
	std::string subj;
	std::string name;
	U32 timestamp;
	BOOL has_attachment;
	U8 asset_type;

	S32 i=0;
	S32 count = msg->getNumberOfBlocks("Data");

	mNoticesList->setEnabled(TRUE);

	for (;i<count;++i)
	{
		msg->getUUID("Data","NoticeID",id,i);
		if (1 == count && id.isNull())
		{
			// Only one entry, the dummy entry.
			mNoticesList->setCommentText(mNoNoticesStr);
			mNoticesList->setEnabled(FALSE);
			return;
		}
			
		msg->getString("Data","Subject",subj,i);
		msg->getString("Data","FromName",name,i);
		msg->getBOOL("Data","HasAttachment",has_attachment,i);
		msg->getU8("Data","AssetType",asset_type,i);
		msg->getU32("Data","Timestamp",timestamp,i);

		// IDEVO clean up legacy "Resident" names
		name = LLCacheName::cleanFullName(name);

		LLSD row;
		row["id"] = id;
		
		row["columns"][0]["column"] = "icon";
		if (has_attachment)
		{
			std::string icon_name = get_item_icon_name(
									(LLAssetType::EType)asset_type,
									LLInventoryType::IT_NONE,FALSE, FALSE);
			row["columns"][0]["type"] = "icon";
			row["columns"][0]["value"] = icon_name;
		}

		row["columns"][1]["column"] = "subject";
		row["columns"][1]["value"] = subj;

		row["columns"][2]["column"] = "from";
		row["columns"][2]["value"] = name;

		row["columns"][3]["column"] = "date";
		row["columns"][3]["value"] = build_notice_date(timestamp);

		row["columns"][4]["column"] = "sort";
		row["columns"][4]["value"] = llformat( "%u", timestamp);

		mNoticesList->addElement(row, ADD_BOTTOM);
	}

	mNoticesList->updateSort();
}

void LLPanelGroupNotices::onSelectNotice(LLUICtrl* ctrl, void* data)
{
	LLPanelGroupNotices* self = (LLPanelGroupNotices*)data;

	if(!self) return;
	LLScrollListItem* item = self->mNoticesList->getFirstSelected();
	if (!item) return;
	
	LLMessageSystem* msg = gMessageSystem;
	msg->newMessage("GroupNoticeRequest");
	msg->nextBlock("AgentData");
	msg->addUUID("AgentID",gAgent.getID());
	msg->addUUID("SessionID",gAgent.getSessionID());
	msg->nextBlock("Data");
	msg->addUUID("GroupNoticeID",item->getUUID());
	gAgent.sendReliableMessage();

	lldebugs << "Item " << item->getUUID() << " selected." << llendl;
}

void LLPanelGroupNotices::showNotice(const std::string& subject,
									 const std::string& message,
									 const bool& has_inventory,
									 const std::string& inventory_name,
									 LLOfferInfo* inventory_offer)
{
	arrangeNoticeView(VIEW_PAST_NOTICE);

	if(mViewSubject) mViewSubject->setText(subject);
	if(mViewMessage) mViewMessage->setText(message);
	
	if (mInventoryOffer)
	{
		// Cancel the inventory offer for the previously viewed notice
		mInventoryOffer->forceResponse(IOR_DECLINE); 
		mInventoryOffer = NULL;
	}

	if (inventory_offer)
	{
		mInventoryOffer = inventory_offer;

		std::string icon_name = get_item_icon_name(mInventoryOffer->mType,
												LLInventoryType::IT_TEXTURE,
												0, FALSE);

		mViewInventoryIcon->setValue(icon_name);
		mViewInventoryIcon->setVisible(TRUE);

		std::stringstream ss;
		ss << "        " << inventory_name;

		mViewInventoryName->setText(ss.str());
		mBtnOpenAttachment->setEnabled(TRUE);
	}
	else
	{
		mViewInventoryName->clear();
		mViewInventoryIcon->setVisible(FALSE);
		mBtnOpenAttachment->setEnabled(FALSE);
	}
}

void LLPanelGroupNotices::arrangeNoticeView(ENoticeView view_type)
{
	if (CREATE_NEW_NOTICE == view_type)
	{
        mPanelCreateNotice->setVisible(TRUE);
		mPanelViewNotice->setVisible(FALSE);
	}
	else
	{
		mPanelCreateNotice->setVisible(FALSE);
		mPanelViewNotice->setVisible(TRUE);
		mBtnOpenAttachment->setEnabled(FALSE);
	}
}
void LLPanelGroupNotices::setGroupID(const LLUUID& id)
{
	sInstances.erase(mGroupID);
	LLPanelGroupTab::setGroupID(id);
	sInstances[mGroupID] = this;

	mBtnNewMessage->setEnabled(gAgent.hasPowerInGroup(mGroupID, GP_NOTICES_SEND));

	LLGroupDropTarget* target = getChild<LLGroupDropTarget> ("drop_target");
	target->setPanel (this);
	target->setGroup (mGroupID);
	
	activate();
}
