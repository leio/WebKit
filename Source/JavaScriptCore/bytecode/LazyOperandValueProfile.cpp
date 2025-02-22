/*
 * Copyright (C) 2012-2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "LazyOperandValueProfile.h"

#include "JSCJSValueInlines.h"

namespace JSC {

CompressedLazyOperandValueProfileHolder::CompressedLazyOperandValueProfileHolder() { }
CompressedLazyOperandValueProfileHolder::~CompressedLazyOperandValueProfileHolder() { }

void CompressedLazyOperandValueProfileHolder::computeUpdatedPredictions(const ConcurrentJSLocker& locker)
{
    if (!m_data)
        return;
    
    for (unsigned i = 0, size = m_data->m_size; i < size; ++i)
        m_data->m_data.at(i).computeUpdatedPrediction(locker);
}

LazyOperandValueProfile* CompressedLazyOperandValueProfileHolder::add(const LazyOperandValueProfileKey& key)
{
    // This addition happens from mutator thread. Thus, we do not need to consider about concurrent additions from multiple threads.
    ASSERT(!isCompilationThread());
    if (!m_data) {
        auto data = makeUnique<LazyOperandValueProfile::Vector>();
        data->m_data.append(LazyOperandValueProfile(key));
        ++data->m_size;
        WTF::storeStoreFence();
        m_data = WTFMove(data);
        return &m_data->m_data.last();
    }

    for (unsigned i = 0, size = m_data->m_size; i < size; ++i) {
        if (m_data->m_data.at(i).key() == key)
            return &m_data->m_data.at(i);
    }
    
    m_data->m_data.append(LazyOperandValueProfile(key));
    WTF::storeStoreFence();
    ++m_data->m_size;
    return &m_data->m_data.last();
}

LazyOperandValueProfileParser::LazyOperandValueProfileParser() { }
LazyOperandValueProfileParser::~LazyOperandValueProfileParser() { }

void LazyOperandValueProfileParser::initialize(CompressedLazyOperandValueProfileHolder& holder)
{
    ASSERT(m_map.isEmpty());

    if (!holder.m_data)
        return;

    auto& data = *holder.m_data;
    for (unsigned i = 0, size = data.m_size; i < size; ++i)
        m_map.add(data.m_data.at(i).key(), &data.m_data.at(i));
}

LazyOperandValueProfile* LazyOperandValueProfileParser::getIfPresent(const LazyOperandValueProfileKey& key) const
{
    auto iter = m_map.find(key);

    if (iter == m_map.end())
        return nullptr;

    return iter->value;
}

SpeculatedType LazyOperandValueProfileParser::prediction(const ConcurrentJSLocker& locker, const LazyOperandValueProfileKey& key) const
{
    LazyOperandValueProfile* profile = getIfPresent(key);
    if (!profile)
        return SpecNone;

    return profile->computeUpdatedPrediction(locker);
}

} // namespace JSC

