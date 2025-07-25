/*
 * IsoffMainParser.cpp
 *****************************************************************************
 * Copyright (C) 2010 - 2012 Klagenfurt University
 *
 * Created on: Jan 27, 2012
 * Authors: Christopher Mueller <christopher.mueller@itec.uni-klu.ac.at>
 *          Christian Timmerer  <christian.timmerer@itec.uni-klu.ac.at>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "IsoffMainParser.h"
#include "../../adaptive/playlist/SegmentTemplate.h"
#include "../../adaptive/playlist/Segment.h"
#include "../../adaptive/playlist/SegmentBase.h"
#include "../../adaptive/playlist/SegmentList.h"
#include "../../adaptive/playlist/SegmentTimeline.h"
#include "../../adaptive/playlist/SegmentInformation.hpp"
#include "../../adaptive/playlist/BasePeriod.h"
#include "MPD.h"
#include "Representation.h"
#include "AdaptationSet.h"
#include "ProgramInformation.h"
#include "DASHSegment.h"
#include "../../adaptive/xml/DOMHelper.h"
#include "../../adaptive/tools/Helper.h"
#include "../../adaptive/tools/Debug.hpp"
#include "../../adaptive/tools/Conversions.hpp"
#include <vlc_stream.h>
#include <cstdio>
#include <limits>

using namespace dash::mpd;
using namespace adaptive::xml;
using namespace adaptive::playlist;

IsoffMainParser::IsoffMainParser    (Node *root_, vlc_object_t *p_object_,
                                     stream_t *stream, const std::string & streambaseurl_)
{
    root = root_;
    p_stream = stream;
    p_object = p_object_;
    playlisturl = streambaseurl_;
}

IsoffMainParser::~IsoffMainParser   ()
{
}

template <class T>
static void parseAvailability(MPD *mpd, Node *node, T *s)
{
    if(node->hasAttribute("availabilityTimeOffset"))
    {
        double val = Integer<double>(node->getAttributeValue("availabilityTimeOffset"));
        s->addAttribute(new AvailabilityTimeOffsetAttr(vlc_tick_from_sec(val)));
    }
    if(node->hasAttribute("availabilityTimeComplete"))
    {
        bool b = (node->getAttributeValue("availabilityTimeComplete") == "false");
        s->addAttribute(new AvailabilityTimeCompleteAttr(!b));
        if(b)
            mpd->setLowLatency(b);
    }
}

void IsoffMainParser::parseMPDBaseUrl(MPD *mpd, Node *root)
{
    std::vector<Node *> baseUrls = DOMHelper::getChildElementByTagName(root, "BaseURL", getDASHNamespace());

    for(size_t i = 0; i < baseUrls.size(); i++)
        mpd->addBaseUrl(baseUrls.at(i)->getText());

    mpd->setPlaylistUrl( Helper::getDirectoryPath(playlisturl).append("/") );
}

MPD * IsoffMainParser::parse()
{
    MPD *mpd = new (std::nothrow) MPD(p_object, getProfile());
    if(mpd)
    {
        parseMPDAttributes(mpd, root);
        parseProgramInformation(DOMHelper::getFirstChildElementByName(root, "ProgramInformation", getDASHNamespace()), mpd);
        parseMPDBaseUrl(mpd, root);
        parsePeriods(mpd, root);
        mpd->addAttribute(new StartnumberAttr(1));
        mpd->debug();
    }
    return mpd;
}

void    IsoffMainParser::parseMPDAttributes   (MPD *mpd, xml::Node *node)
{
    const Node::Attributes& attributes = node->getAttributes();

    mpd->b_needsUpdates = false;

    for(auto attr: attributes)
    {
        if(attr.name == "mediaPresentationDuration")
        {
            mpd->duration = IsoTime(attr.value);
        }
        else if(attr.name == "minBufferTime")
        {
            mpd->setMinBuffering(IsoTime(attr.value));
        }
        else if(attr.name == "minimumUpdatePeriod")
        {
            mpd->b_needsUpdates = true;
            vlc_tick_t minupdate = IsoTime(attr.value);
            if(minupdate > 0)
                mpd->minUpdatePeriod = minupdate;
        }
        else if(attr.name == "maxSegmentDuration")
        {
            mpd->maxSegmentDuration = IsoTime(attr.value);
        }
        else if(attr.name == "type")
        {
            mpd->setType(attr.value);
        }
        else if(attr.name == "availabilityStartTime")
        {
            mpd->availabilityStartTime = UTCTime(attr.value);
        }
        else if(attr.name == "availabilityEndTime")
        {
            mpd->availabilityEndTime = UTCTime(attr.value);
        }
        else if(attr.name == "timeShiftBufferDepth")
        {
            mpd->timeShiftBufferDepth = IsoTime(attr.value);
        }
        else if(attr.name == "suggestedPresentationDelay")
        {
            mpd->suggestedPresentationDelay = IsoTime(attr.value);
        }
    }
}

void IsoffMainParser::parsePeriods(MPD *mpd, Node *root)
{
    std::vector<Node *> periods = DOMHelper::getElementByTagName(root, "Period", getDASHNamespace(), false);
    std::vector<Node *>::const_iterator it;
    uint64_t nextid = 0;

    for(it = periods.begin(); it != periods.end(); ++it)
    {
        BasePeriod *period = new (std::nothrow) BasePeriod(mpd);
        if (!period)
            continue;
        parseSegmentInformation(mpd, *it, period, &nextid);
        if((*it)->hasAttribute("start"))
            period->startTime = IsoTime((*it)->getAttributeValue("start"));
        if((*it)->hasAttribute("duration"))
            period->duration = IsoTime((*it)->getAttributeValue("duration"));
        std::vector<Node *> baseUrls = DOMHelper::getChildElementByTagName(*it, "BaseURL", getDASHNamespace());
        if(!baseUrls.empty())
        {
            period->baseUrl = new Url( baseUrls.front()->getText() );
            parseAvailability<BasePeriod>(mpd, baseUrls.front(), period);
        }

        parseAdaptationSets(mpd, *it, period);
        mpd->addPeriod(period);
    }
}

void IsoffMainParser::parseSegmentBaseType(MPD *, Node *node,
                                           AbstractSegmentBaseType *base,
                                           SegmentInformation *parent)
{
    parseInitSegment(DOMHelper::getFirstChildElementByName(node, "Initialization", getDASHNamespace()), base, parent);

    if(node->hasAttribute("indexRange"))
    {
        size_t start = 0, end = 0;
        if (std::sscanf(node->getAttributeValue("indexRange").c_str(), "%zu-%zu", &start, &end) == 2)
        {
            IndexSegment *index = new (std::nothrow) DashIndexSegment(parent);
            if(index)
            {
                index->setByteRange(start, end);
                base->indexSegment = index;
                /* index must be before data, so data starts at index end */
                if(dynamic_cast<SegmentBase *>(base))
                    dynamic_cast<SegmentBase *>(base)->setByteRange(end + 1, 0);
            }
        }
    }

    if(node->hasAttribute("timescale"))
    {
        TimescaleAttr *prop = new TimescaleAttr(Timescale(Integer<uint64_t>(node->getAttributeValue("timescale"))));
        base->addAttribute(prop);
    }
}

void IsoffMainParser::parseMultipleSegmentBaseType(MPD *mpd, Node *node,
                                                   AbstractMultipleSegmentBaseType *base,
                                                   SegmentInformation *parent)
{
    parseSegmentBaseType(mpd, node, base, parent);

    if(node->hasAttribute("duration"))
        base->addAttribute(new DurationAttr(Integer<stime_t>(node->getAttributeValue("duration"))));

    if(node->hasAttribute("startNumber"))
        base->addAttribute(new StartnumberAttr(Integer<uint64_t>(node->getAttributeValue("startNumber"))));

    parseTimeline(DOMHelper::getFirstChildElementByName(node, "SegmentTimeline", getDASHNamespace()), base);
}

size_t IsoffMainParser::parseSegmentTemplate(MPD *mpd, Node *templateNode, SegmentInformation *info)
{
    size_t total = 0;
    if (templateNode == nullptr)
        return total;

    std::string mediaurl;
    if(templateNode->hasAttribute("media"))
        mediaurl = templateNode->getAttributeValue("media");

    SegmentTemplate *mediaTemplate = new (std::nothrow) SegmentTemplate(new SegmentTemplateSegment(), info);
    if(!mediaTemplate)
        return total;
    mediaTemplate->setSourceUrl(mediaurl);

    parseMultipleSegmentBaseType(mpd, templateNode, mediaTemplate, info);

    parseAvailability<SegmentInformation>(mpd, templateNode, info);

    if(templateNode->hasAttribute("initialization")) /* /!\ != Initialization */
    {
        SegmentTemplateInit *initTemplate;
        const std::string &initurl = templateNode->getAttributeValue("initialization");
        if(!initurl.empty() && (initTemplate = new (std::nothrow) SegmentTemplateInit(mediaTemplate, info)))
        {
            initTemplate->setSourceUrl(initurl);
            delete mediaTemplate->initialisationSegment;
            mediaTemplate->initialisationSegment = initTemplate;
        }
    }

    info->setSegmentTemplate(mediaTemplate);

    return mediaurl.empty() ? ++total : 0;
}

size_t IsoffMainParser::parseSegmentInformation(MPD *mpd, Node *node,
                                                SegmentInformation *info, uint64_t *nextid)
{
    size_t total = 0;
    total += parseSegmentBase(mpd, DOMHelper::getFirstChildElementByName(node, "SegmentBase", getDASHNamespace()), info);
    total += parseSegmentList(mpd, DOMHelper::getFirstChildElementByName(node, "SegmentList", getDASHNamespace()), info);
    total += parseSegmentTemplate(mpd, DOMHelper::getFirstChildElementByName(node, "SegmentTemplate", getDASHNamespace()), info);
    if(node->hasAttribute("timescale"))
        info->addAttribute(new TimescaleAttr(Timescale(Integer<uint64_t>(node->getAttributeValue("timescale")))));

    parseAvailability<SegmentInformation>(mpd, node, info);

    if(node->hasAttribute("id"))
        info->setID(ID(node->getAttributeValue("id")));
    else
        info->setID(ID((*nextid)++));

    return total;
}

void    IsoffMainParser::parseAdaptationSets  (MPD *mpd, Node *periodNode, BasePeriod *period)
{
    std::vector<Node *> adaptationSets = DOMHelper::getElementByTagName(periodNode, "AdaptationSet", getDASHNamespace(), false);
    std::vector<Node *>::const_iterator it;
    uint64_t nextid = 0;

    for(it = adaptationSets.begin(); it != adaptationSets.end(); ++it)
    {
        AdaptationSet *adaptationSet = new (std::nothrow) AdaptationSet(period);
        if(!adaptationSet)
            continue;

        parseCommonAttributesElements(*it, adaptationSet);

        if((*it)->hasAttribute("lang"))
            adaptationSet->setLang((*it)->getAttributeValue("lang"));

        if((*it)->hasAttribute("bitstreamSwitching"))
            adaptationSet->setBitswitchAble((*it)->getAttributeValue("bitstreamSwitching") == "true");

        if((*it)->hasAttribute("segmentAlignment"))
            adaptationSet->setSegmentAligned((*it)->getAttributeValue("segmentAlignment") == "true");

        Node *baseUrl = DOMHelper::getFirstChildElementByName((*it), "BaseURL", getDASHNamespace());
        if(baseUrl)
        {
            parseAvailability<AdaptationSet>(mpd, baseUrl, adaptationSet);
            adaptationSet->baseUrl = new Url(baseUrl->getText());
        }

        Node *role = DOMHelper::getFirstChildElementByName((*it), "Role", getDASHNamespace());
        if(role && role->hasAttribute("schemeIdUri") && role->hasAttribute("value"))
        {
            const std::string &uri = role->getAttributeValue("schemeIdUri");
            if(uri == "urn:mpeg:dash:role:2011")
            {
                const std::string &rolevalue = role->getAttributeValue("value");
                adaptationSet->description = rolevalue;
                if(rolevalue == "main")
                    adaptationSet->setRole(Role::Value::Main);
                else if(rolevalue == "alternate")
                    adaptationSet->setRole(Role::Value::Alternate);
                else if(rolevalue == "supplementary")
                    adaptationSet->setRole(Role::Value::Supplementary);
                else if(rolevalue == "commentary")
                    adaptationSet->setRole(Role::Value::Commentary);
                else if(rolevalue == "dub")
                    adaptationSet->setRole(Role::Value::Dub);
                else if(rolevalue == "caption")
                    adaptationSet->setRole(Role::Value::Caption);
                else if(rolevalue == "subtitle")
                    adaptationSet->setRole(Role::Value::Subtitle);
            }
        }

        parseSegmentInformation(mpd, *it, adaptationSet, &nextid);

        parseRepresentations(mpd, (*it), adaptationSet);

#ifdef ADAPTATIVE_ADVANCED_DEBUG
        if(adaptationSet->description.empty())
            adaptationSet->description.Set(adaptationSet->getID().str());
#endif

        if(!adaptationSet->getRepresentations().empty())
            period->addAdaptationSet(adaptationSet);
        else
            delete adaptationSet;
    }
}

void IsoffMainParser::parseCommonAttributesElements(Node *node,
                                                    CommonAttributesElements *commonAttrElements)
{
    if(node->hasAttribute("width"))
        commonAttrElements->setWidth(atoi(node->getAttributeValue("width").c_str()));

    if(node->hasAttribute("height"))
        commonAttrElements->setHeight(atoi(node->getAttributeValue("height").c_str()));

    if(node->hasAttribute("mimeType"))
        commonAttrElements->setMimeType(node->getAttributeValue("mimeType"));
}

void    IsoffMainParser::parseRepresentations (MPD *mpd, Node *adaptationSetNode, AdaptationSet *adaptationSet)
{
    std::vector<Node *> representations = DOMHelper::getElementByTagName(adaptationSetNode, "Representation", getDASHNamespace(), false);
    uint64_t nextid = 0;

    for(size_t i = 0; i < representations.size(); i++)
    {
        Representation *currentRepresentation = new Representation(adaptationSet);
        Node *repNode = representations.at(i);

        std::vector<Node *> baseUrls = DOMHelper::getChildElementByTagName(repNode, "BaseURL", getDASHNamespace());
        if(!baseUrls.empty())
        {
            currentRepresentation->baseUrl = new Url(baseUrls.front()->getText());
            parseAvailability<Representation>(mpd, baseUrls.front(), currentRepresentation);
        }

        if(repNode->hasAttribute("id"))
            currentRepresentation->setID(ID(repNode->getAttributeValue("id")));

        parseCommonAttributesElements(repNode, currentRepresentation);

        if(repNode->hasAttribute("bandwidth"))
            currentRepresentation->setBandwidth(atoi(repNode->getAttributeValue("bandwidth").c_str()));

        if(repNode->hasAttribute("codecs"))
            currentRepresentation->addCodecs(repNode->getAttributeValue("codecs"));

        size_t i_total = parseSegmentInformation(mpd, repNode, currentRepresentation, &nextid);
        /* Empty Representation with just baseurl (ex: subtitles) */
        if(i_total == 0 &&
           (currentRepresentation->baseUrl && !currentRepresentation->baseUrl->empty()) &&
            adaptationSet->getMediaSegment(0) == nullptr)
        {
            SegmentBase *base = new (std::nothrow) SegmentBase(currentRepresentation);
            if(base)
                currentRepresentation->addAttribute(base);
        }

        adaptationSet->addRepresentation(currentRepresentation);
    }
}
size_t IsoffMainParser::parseSegmentBase(MPD *mpd, Node * segmentBaseNode, SegmentInformation *info)
{
    SegmentBase *base;

    if(!segmentBaseNode || !(base = new (std::nothrow) SegmentBase(info)))
        return 0;

    parseSegmentBaseType(mpd, segmentBaseNode, base, info);

    parseAvailability<SegmentInformation>(mpd, segmentBaseNode, info);

    if(!base->initialisationSegment && base->indexSegment && base->indexSegment->getOffset())
    {
        InitSegment *initSeg = new InitSegment( info );
        initSeg->setSourceUrl(base->getUrlSegment().toString());
        initSeg->setByteRange(0, base->indexSegment->getOffset() - 1);
        base->initialisationSegment = initSeg;
    }

    info->addAttribute(base);

    return 1;
}

size_t IsoffMainParser::parseSegmentList(MPD *mpd, Node * segListNode, SegmentInformation *info)
{
    size_t total = 0;
    if(segListNode)
    {
        std::vector<Node *> segments = DOMHelper::getElementByTagName(segListNode, "SegmentURL", getDASHNamespace(), false);
        SegmentList *list;
        if((list = new (std::nothrow) SegmentList(info)))
        {
            parseMultipleSegmentBaseType(mpd, segListNode, list, info);

            parseAvailability<SegmentInformation>(mpd, segListNode, info);

            uint64_t sequenceNumber = info->inheritStartNumber();
            if(sequenceNumber == std::numeric_limits<uint64_t>::max())
                sequenceNumber = 0;

            const stime_t duration = list->inheritDuration();
            stime_t nzStartTime = sequenceNumber * duration;
            std::vector<Node *>::const_iterator it;
            for(it = segments.begin(); it != segments.end(); ++it)
            {
                Node *segmentURL = *it;

                Segment *seg = new (std::nothrow) Segment(info);
                if(!seg)
                    continue;

                const std::string &mediaUrl = segmentURL->getAttributeValue("media");
                if(!mediaUrl.empty())
                    seg->setSourceUrl(mediaUrl);

                if(segmentURL->hasAttribute("mediaRange"))
                {
                    const std::string &range = segmentURL->getAttributeValue("mediaRange");
                    size_t pos = range.find("-");
                    seg->setByteRange(atoi(range.substr(0, pos).c_str()), atoi(range.substr(pos + 1, range.size()).c_str()));
                }

                seg->startTime = nzStartTime;
                seg->duration = duration;
                nzStartTime += duration;

                seg->setSequenceNumber(sequenceNumber++);

                list->addSegment(seg);
            }

            total = list->getSegments().size();
            info->updateSegmentList(list, true);
        }
    }
    return total;
}

void IsoffMainParser::parseInitSegment(Node *initNode, Initializable<InitSegment> *init, SegmentInformation *parent)
{
    if(!initNode)
        return;

    InitSegment *seg = new InitSegment( parent );
    seg->setSourceUrl(initNode->getAttributeValue("sourceURL"));

    if(initNode->hasAttribute("range"))
    {
        const std::string &range = initNode->getAttributeValue("range");
        size_t pos = range.find("-");
        seg->setByteRange(atoi(range.substr(0, pos).c_str()), atoi(range.substr(pos + 1, range.size()).c_str()));
    }

    init->initialisationSegment = seg;
}

void IsoffMainParser::parseTimeline(Node *node, AbstractMultipleSegmentBaseType *base)
{
    if(!node)
        return;

    uint64_t number = 0;
    if(node->hasAttribute("startNumber"))
        number = Integer<uint64_t>(node->getAttributeValue("startNumber"));
    else if(base->inheritStartNumber())
        number = base->inheritStartNumber();
    if(number == std::numeric_limits<uint64_t>::max())
        number = 1;

    SegmentTimeline *timeline = new (std::nothrow) SegmentTimeline(base);
    if(timeline)
    {
        std::vector<Node *> elements = DOMHelper::getElementByTagName(node, "S", getDASHNamespace(), false);
        std::vector<Node *>::const_iterator it;
        for(it = elements.begin(); it != elements.end(); ++it)
        {
            const Node *s = *it;
            if(!s->hasAttribute("d")) /* Mandatory */
                continue;
            stime_t d = Integer<stime_t>(s->getAttributeValue("d"));
            int64_t r = 0; // never repeats by default
            if(s->hasAttribute("r"))
            {
                r = Integer<int64_t>(s->getAttributeValue("r"));
                if(r < 0)
                    r = std::numeric_limits<unsigned>::max();
            }

            if(s->hasAttribute("t"))
            {
                stime_t t = Integer<stime_t>(s->getAttributeValue("t"));
                timeline->addElement(number, d, r, t);
            }
            else timeline->addElement(number, d, r);

            number += (1 + r);
        }
        //base->setSegmentTimeline(timeline);
        base->addAttribute(timeline);
    }
}

void IsoffMainParser::parseProgramInformation(Node * node, MPD *mpd)
{
    if(!node)
        return;

    ProgramInformation *info = new (std::nothrow) ProgramInformation();
    if (info)
    {
        Node *child = DOMHelper::getFirstChildElementByName(node, "Title", getDASHNamespace());
        if(child)
            info->setTitle(child->getText());

        child = DOMHelper::getFirstChildElementByName(node, "Source", getDASHNamespace());
        if(child)
            info->setSource(child->getText());

        child = DOMHelper::getFirstChildElementByName(node, "Copyright", getDASHNamespace());
        if(child)
            info->setCopyright(child->getText());

        if(node->hasAttribute("moreInformationURL"))
            info->setMoreInformationUrl(node->getAttributeValue("moreInformationURL"));

        mpd->programInfo = info;
    }
}

Profile IsoffMainParser::getProfile() const
{
    Profile res(Profile::Name::Unknown);
    if(this->root == nullptr)
        return res;

    std::string urn = root->getAttributeValue("profiles");
    if ( urn.length() == 0 )
        urn = root->getAttributeValue("profile"); //The standard spells it the both ways...

    size_t pos;
    size_t nextpos = std::string::npos;
    do
    {
        pos = nextpos + 1;
        nextpos = urn.find_first_of(",", pos);
        res = Profile(urn.substr(pos, nextpos - pos));
    }
    while (nextpos != std::string::npos && res == Profile::Name::Unknown);

    return res;
}

const std::string & IsoffMainParser::getDASHNamespace() const
{
    return root->getNamespace();
}
