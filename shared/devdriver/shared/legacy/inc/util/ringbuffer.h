/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2023 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/

#pragma once

#include <ddPlatform.h>
#include <ddAmdLogInterface.h>

namespace DevDriver
{
class RingBuffer
{
public:

    RingBuffer(size_t size, const AllocCb& allocCb)
        : m_ringBuffer(allocCb),
          m_size(size),
          m_spaceUsed(0),
          m_writePointer(0),
          m_readPointer(0),
          m_mutex()
    {
    }

    ~RingBuffer()
    {
    }

    Result Init()
    {
        m_ringBuffer.ResizeAndZero(m_size);

        return Result::Success;
    }

    // Writes data to the ring buffer.
    // This function will overwrite the oldest data when there is no additional room.
    // The function will return InvalidParameter and skip the write when:
    //      1. The event + dataSize exceed the total buffer size.
    //      2. The event or pData are null
    Result WriteData(const AmdLogEventInfo* pEvent)
    {
        Result result = Result::InvalidParameter;
        m_mutex.Lock();

        if ((pEvent != nullptr) && (pEvent->pData != nullptr))
        {
            size_t sizeNeeded = sizeof(AmdLogEventInfo) + pEvent->dataSize;

            if (sizeNeeded < m_size)
            {
                while (sizeNeeded > (m_size - m_spaceUsed))
                {
                    FreeSpace();
                }

                // Write event object and then copy the data immediately after that
                WriteRawData(pEvent, sizeof(AmdLogEventInfo));
                WriteRawData(pEvent->pData, pEvent->dataSize);

                result = Result::Success;
            }
        }

        m_mutex.Unlock();

        return result;
    }

    // Reads back the last event.
    // The data size can be queried using the peek method
    // This will return InvalidParameter and not read the data if:
    //     1. The event or pData is null
    //     2. The dataSize exceeds the total size.
    //     3. No events have been written
    Result ReadData(AmdLogEventInfo* pRetEvent)
    {
        Result result = Result::InvalidParameter;
        if ((pRetEvent != nullptr)           &&
            (pRetEvent->pData != nullptr)    &&
            (pRetEvent->dataSize <= m_size)  &&
            (m_spaceUsed > 0))
        {
            m_mutex.Lock();
            // We will clober the event when we read it, so store off the pointer to the buffer
            void* pData = pRetEvent->pData;
            ReadRawData(pRetEvent, sizeof(AmdLogEventInfo), true);
            pRetEvent->pData = pData;
            ReadRawData(pRetEvent->pData, pRetEvent->dataSize);
            m_mutex.Unlock();
            result = Result::Success;
        }

        return result;
    }

    // Routine to read the data into a packed buffer.
    // The format is the header followed by the event payload.
    // The pointers to the data will be invalid as the data is packed immediately after the event
    // The size of the buffer to allocate should be queried by calling GetSpaceUsed().
    // If no data has been writen, the behavior is undefined
    Result ReadPackedBuffer(void* pBuffer, size_t len)
    {
        Result result = Result::InvalidParameter;
        if ((pBuffer != nullptr) && (len <= m_size))
        {
            m_mutex.Lock();
            ReadRawData(pBuffer, len, true);
            m_mutex.Unlock();
            result = Result::Success;
        }

        return result;
    }

    // Used to peek at the size of the first event.
    // This will return 0 when there are no unread events
    size_t PeekEventDataSize()
    {
        m_mutex.Lock();
        // Create a temporary event and copy the ring buffer data to it:
        AmdLogEventInfo event = {};
        if (m_spaceUsed > 0)
        {
            ReadRawData(&event, sizeof(AmdLogEventInfo), false);
        }
        m_mutex.Unlock();

        return event.dataSize;
    }

    size_t GetSpaceUsed() const
    {
        return m_spaceUsed;
    }

private:

    // Routine for writing raw data to the ring buffer
    // Note: This routine assumes a lock has been taken and the pointer is valid
    void WriteRawData(const void* pBuffer, size_t len)
    {
        DD_ASSERT(pBuffer != nullptr);
        char*  pRbData    = m_ringBuffer.Data();
        size_t tailLength = Platform::Min(m_size - m_writePointer, len);
        memcpy(&pRbData[m_writePointer], pBuffer, tailLength);
        memcpy(pRbData, (char*)pBuffer + tailLength, len - tailLength);
        m_writePointer = (len + m_writePointer < m_size) ? len + m_writePointer : len + m_writePointer - m_size;
        m_spaceUsed += len;
    }

    // Routine for reading raw data from the ring buffer
    // Note: This routine assumes a lock has been taken and the pointer is valid
    void ReadRawData(void* pBuffer, size_t len, bool advancePtr = true)
    {
        DD_ASSERT(pBuffer != nullptr);
        size_t tailLength = Platform::Min(m_size - m_readPointer, len);
        memcpy(pBuffer, &m_ringBuffer[m_readPointer], tailLength);
        memcpy((char*)pBuffer + tailLength, &m_ringBuffer[0], len - tailLength);

        if (advancePtr)
        {
            m_readPointer = (len + m_readPointer < m_size) ? len + m_readPointer : len + m_readPointer - m_size;
            m_spaceUsed -= len;
        }
    }

    // Clears one space from the ring buffer and advances the read pointer
    // Note: This routine assumes a lock has been taken
    void FreeSpace()
    {
        char* pData                 = m_ringBuffer.Data();
        AmdLogEventInfo* pEvent     = (AmdLogEventInfo*)&pData[m_readPointer];
        size_t           len        = sizeof(AmdLogEventInfo) + pEvent->dataSize;
        size_t           tailLength = Platform::Min(m_size - m_readPointer, len);
        memset(&pData[m_readPointer], 0, tailLength);
        memset(pData, 0, len - tailLength);
        m_readPointer = (len + m_readPointer < m_size) ? len + m_readPointer : len + m_readPointer - m_size;
        m_spaceUsed  -= len;
    }

    Vector<char>      m_ringBuffer;   // Ring buffer memory.
    const size_t      m_size;         // Size of the ring buffer.
    size_t            m_spaceUsed;    // Amount of space currently used.
    size_t            m_writePointer; // The write pointer of the ring buffer.
    size_t            m_readPointer;  // The read pointer of the ring buffer.
    Platform::Mutex   m_mutex;        // Mutex for access to the ring buffer.
};

} // DevDriver
