/*
 *
 * Copyright (C) 2009-2014 Halon Security <support@halon.se>
 *
 * This file is part of libdkim++.
 *
 * libdkim++ is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * libdkim++ is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with libdkim++.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include "Signatory.hpp"

using DKIM::Signatory;

#include "Canonicalization.hpp"

using DKIM::Conversion::CanonicalizationHeader;
using DKIM::Conversion::CanonicalizationBody;

#include "Base64.hpp"

using DKIM::Conversion::Base64_Encode;

#include "Util.hpp"

using DKIM::Util::Algorithm2String;
using DKIM::Util::CanonMode2String;
using DKIM::Util::StringFormat;

#include <algorithm>
#include <map>
#include <set>

Signatory::Signatory(std::istream& file)
: m_file(file)
{
}

Signatory::~Signatory()
{
}

std::string Signatory::CreateSignature(const SignatoryOptions& options)
	throw (DKIM::PermanentError)
{
	while (m_msg.ParseLine(m_file) && !m_msg.IsDone()) { }

	// create signature for our body (message data)
	EVP_MD_CTX evpmdbody;
	switch (options.GetAlgorithm())
	{
		case DKIM::DKIM_A_SHA1:
			EVP_DigestInit(&evpmdbody, EVP_sha1());
			break;
		case DKIM::DKIM_A_SHA256:
			EVP_DigestInit(&evpmdbody, EVP_sha256());
			break;
	}

	DKIM::Conversion::EVPDigest evpupd;
	evpupd.ctx = &evpmdbody;

	if (!CanonicalizationBody(m_file,
			options.GetCanonModeBody(),
			m_msg.GetBodyOffset(),
			options.GetBodySignLength(),
			options.GetBodyLength(),
			std::bind(&DKIM::Conversion::EVPDigest::update, &evpupd, std::placeholders::_1, std::placeholders::_2)))
		throw DKIM::PermanentError("Body sign limit exceed the size of the canonicalized message length");

	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int md_len;
	EVP_DigestFinal(&evpmdbody, md, &md_len);

	std::string bh((char*)md, md_len);

	// create signature for our header
	EVP_MD_CTX evpmdhead;
	int md_nid;
	switch (options.GetAlgorithm())
	{
		case DKIM::DKIM_A_SHA1:
			EVP_DigestInit(&evpmdhead, EVP_sha1());
			md_nid = NID_sha1;
			break;
		case DKIM::DKIM_A_SHA256:
			EVP_DigestInit(&evpmdhead, EVP_sha256());
			md_nid = NID_sha256;
			break;
	}

	CanonicalizationHeader canonicalhead(options.GetCanonModeHeader());

	std::set<std::string> headersToSign;
	for (auto name : options.GetHeaders())
	{
		transform(name.begin(), name.end(), name.begin(), tolower);
		headersToSign.insert(name);
	}

	std::list<std::string> signedHeaders;

	bool signAll = false;
	if (headersToSign.empty()) signAll = true;

	// add all headers to our cache (they will be pop of the end)
	const auto & headers = m_msg.GetHeaders();
	for (auto h = headers.rbegin(); h != headers.rend(); ++h)
	{
		std::string name = (*h)->GetName();
		transform(name.begin(), name.end(), name.begin(), tolower);
		if (!name.empty())
		{
			if (!signAll && headersToSign.find(name) == headersToSign.end())
				continue;
			std::string tmp = canonicalhead.FilterHeader((*h)->GetHeader()) + "\r\n";
			if (!tmp.empty())
			{
				EVP_DigestUpdate(&evpmdhead, tmp.c_str(), tmp.size());
				signedHeaders.push_back(name);
			}
		}
	}

	std::string dkimHeader;
	unsigned long arcInstance = options.GetARCInstance();
	if (arcInstance)
		dkimHeader += "ARC-Message-Signature: i=" + StringFormat("%lu", arcInstance) + "; a=" + Algorithm2String(options.GetSignatureAlgorithm(), options.GetAlgorithm()) + "; c="
					+ CanonMode2String(options.GetCanonModeHeader()) + "/" + CanonMode2String(options.GetCanonModeBody()) + ";\r\n";
	else
		dkimHeader += "DKIM-Signature: v=1; a=" + Algorithm2String(options.GetSignatureAlgorithm(), options.GetAlgorithm()) + "; c="
					+ CanonMode2String(options.GetCanonModeHeader()) + "/" + CanonMode2String(options.GetCanonModeBody()) + ";\r\n";

	std::string limit;
	if (options.GetBodySignLength())
		limit = StringFormat("; l=%lu", options.GetBodyLength());

	dkimHeader += "\td=" + options.GetDomain() + "; s=" + options.GetSelector() + limit + ";\r\n";

	std::string headerlist = "\th=";
	for (std::list<std::string>::const_iterator i = signedHeaders.begin();
		i != signedHeaders.end(); ++i)
	{
		bool insertColon = (i != signedHeaders.begin());
		if (headerlist.size() + i->size() + (insertColon?1:0) > 80)
		{
			dkimHeader += headerlist + (insertColon?":":"") + "\r\n";
			headerlist = "\t " + *i;
		} else {
			headerlist += (insertColon?":":"") + *i;
		}
	}
	dkimHeader += headerlist + ";\r\n";
	dkimHeader += "\tbh=" + Base64_Encode(bh) + ";\r\n";
	dkimHeader += "\tb=";

	std::string tmp2 = canonicalhead.FilterHeader(dkimHeader);
	EVP_DigestUpdate(&evpmdhead, tmp2.c_str(), tmp2.size());
	EVP_DigestFinal(&evpmdhead, md, &md_len);

    RSA* rsa = EVP_PKEY_get1_RSA(options.GetPrivateKey());

    unsigned int sig_len;
    unsigned char* sig = new unsigned char[RSA_size(rsa)];

    int r = RSA_sign(md_nid,
            md,
            md_len,
            sig,
            &sig_len,
            rsa);

    RSA_free(rsa);

	if (r != 1)
	{
		delete [] sig;
		throw DKIM::PermanentError("Message could not be signed");
	}

	std::string tmp3((const char*)sig, sig_len);
	delete [] sig;

	size_t offset = 3; // "\tb=";
	std::string split = Base64_Encode(tmp3);
	while (!split.empty())
	{
		dkimHeader += split.substr(0, 80 - offset);
		split.erase(0, 80 - offset);
		if (!split.empty())
			dkimHeader += "\r\n\t ";
		offset = 2; // "\t ";
	}

	return dkimHeader;
}
