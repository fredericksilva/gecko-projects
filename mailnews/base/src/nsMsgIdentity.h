/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

#ifndef nsMsgIdentity_h___
#define nsMsgIdentity_h___

#include "nsIMsgIdentity.h"

///////////////////////////////////////////////////////////////////////////////////
// an identity is an object designed to encapsulate all the information we need 
// to know about a user identity. I expect this interface to grow and change a lot
// as we flesh out our thoughts on multiple identities and what properties go into
// these identities.
//////////////////////////////////////////////////////////////////////////////////

class nsMsgIdentity : public nsIMsgIdentity
{
public:
	nsMsgIdentity();
	virtual ~nsMsgIdentity();

	NS_DECL_ISUPPORTS

	///////////////////////////////////////////////////////////////////////////////////
	// The user's current identity is used to abstract out information such as the
	// user name, pwd, mail server to use, etc....
	//////////////////////////////////////////////////////////////////////////////////
	NS_IMETHOD GetRootFolderPath(const char ** aRootFolderPath);
	NS_IMETHOD GetUserName(const char ** aUserName); // right now it is pop & smtp user name
	NS_IMETHOD GetPopPassword(const char ** aUserPassword);
	NS_IMETHOD GetPopServer(const char ** aHostName);
	NS_IMETHOD GetSmtpServer(const char ** aHostName);

protected:
	char	*m_userName;
	char	*m_userPassword;
	char	*m_smtpHost;
	char	*m_popHost;
	char	*m_rootPath;

	void InitializeIdentity();
};

#endif /* nsMsgIdentity_h___ */
