/*
 * MIT License
 *
 * Copyright (c) 2018 Michele Biondi, Andrea Salvatori
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
*/

#include <Arduino.h>
#include "DW1000NgRTLS.hpp"
#include "DW1000Ng.hpp"
#include "DW1000NgUtils.hpp"
#include "DW1000NgTime.hpp"
#include "DW1000NgRanging.hpp"

static byte SEQ_NUMBER = 0;

namespace DW1000NgRTLS {

    byte increaseSequenceNumber(){
        return ++SEQ_NUMBER;
    }

    void transmitTwrShortBlink() {
        byte Blink[] = {BLINK, SEQ_NUMBER++, 0,0,0,0,0,0,0,0, NO_BATTERY_STATUS | NO_EX_ID, TAG_LISTENING_NOW};
        DW1000Ng::getEUI(&Blink[2]);
        DW1000Ng::setTransmitData(Blink, sizeof(Blink));
        DW1000Ng::startTransmit();
    }

    void transmitPoll(byte anchor_address[]){
        byte Poll[] = {DATA, SHORT_SRC_AND_DEST, SEQ_NUMBER++, 0,0, 0,0, 0,0 , RANGING_TAG_POLL};
        DW1000Ng::getNetworkId(&Poll[3]);
        memcpy(&Poll[5], anchor_address, 2);
        DW1000Ng::getDeviceAddress(&Poll[7]);
        DW1000Ng::setTransmitData(Poll, sizeof(Poll));
        DW1000Ng::startTransmit();
    }

    void transmitFinalMessage(byte anchor_address[], uint16_t reply_delay, uint64_t timePollSent, uint64_t timeResponseToPollReceived) {
        /* Calculation of future time */
        byte futureTimeBytes[LENGTH_TIMESTAMP];

        uint64_t timeFinalMessageSent = DW1000Ng::getSystemTimestamp();
        timeFinalMessageSent += DW1000NgTime::microsecondsToUWBTime(reply_delay);
        DW1000NgUtils::writeValueToBytes(futureTimeBytes, timeFinalMessageSent, LENGTH_TIMESTAMP);
        DW1000Ng::setDelayedTRX(futureTimeBytes);
        timeFinalMessageSent += DW1000Ng::getTxAntennaDelay();

        byte finalMessage[] = {DATA, SHORT_SRC_AND_DEST, SEQ_NUMBER++, 0,0, 0,0, 0,0, RANGING_TAG_FINAL_RESPONSE_EMBEDDED,
                               0,0,0,0,0,0,0,0,0,0,0,0
        };

        DW1000Ng::getNetworkId(&finalMessage[3]);
        memcpy(&finalMessage[5], anchor_address, 2);
        DW1000Ng::getDeviceAddress(&finalMessage[7]);

        DW1000NgUtils::writeValueToBytes(finalMessage + 10, (uint32_t) timePollSent, 4);
        DW1000NgUtils::writeValueToBytes(finalMessage + 14, (uint32_t) timeResponseToPollReceived, 4);
        DW1000NgUtils::writeValueToBytes(finalMessage + 18, (uint32_t) timeFinalMessageSent, 4);
        DW1000Ng::setTransmitData(finalMessage, sizeof(finalMessage));
        DW1000Ng::startTransmit(TransmitMode::DELAYED);
    }

    static uint32_t calculateNewBlinkRate(byte frame[]) {
        uint32_t blinkRate = frame[11] + static_cast<uint32_t>(((frame[12] & 0x3F) << 8));
        byte multiplier = ((frame[12] & 0xC0) >> 6);
        if(multiplier  == 0x01) {
            blinkRate *= 25;
        } else if(multiplier == 0x02) {
            blinkRate *= 1000;
        }

        return blinkRate;
    }

    void waitForTransmission() {
        while(!DW1000Ng::isTransmitDone()) ;
        DW1000Ng::clearTransmitStatus();
    }

    boolean receiveFrame() {
        DW1000Ng::startReceive();
        while(!DW1000Ng::isReceiveDone()) {
            if(DW1000Ng::isReceiveTimeout() ) {
                DW1000Ng::clearReceiveTimeoutStatus();
                return false;
            }
        }
        DW1000Ng::clearReceiveStatus();
        return true;
    }

    static boolean waitForNextRangingStep() {
        DW1000NgRTLS::waitForTransmission();
        if(!DW1000NgRTLS::receiveFrame()) return false;
        return true;
    }

    RangeRequestResult tagRangeRequest() {
        DW1000NgRTLS::transmitTwrShortBlink();

        if(!DW1000NgRTLS::waitForNextRangingStep()) return {false, 0};

        size_t init_len = DW1000Ng::getReceivedDataLength();
        byte init_recv[init_len];
        DW1000Ng::getReceivedData(init_recv, init_len);

        if(!(init_len > 17 && init_recv[15] == RANGING_INITIATION)) {
            return { false, 0};
        }

        return { true, DW1000NgUtils::bytesAsValue(&init_recv[13], 2) };
    }

    static RangeResult tagFinishRange(uint16_t anchor, uint16_t replyDelayUs) {
        byte target_anchor[2];
        DW1000NgUtils::writeValueToBytes(target_anchor, anchor, 2);
        DW1000NgRTLS::transmitPoll(target_anchor);
        /* Start of poll control for range */
        if(!DW1000NgRTLS::waitForNextRangingStep()) return {false, false, 0, 0};
        size_t cont_len = DW1000Ng::getReceivedDataLength();
        byte cont_recv[cont_len];
        DW1000Ng::getReceivedData(cont_recv, cont_len);

        if (cont_len > 10 && cont_recv[9] == ACTIVITY_CONTROL && cont_recv[10] == RANGING_CONTINUE) {
            /* Received Response to poll */
            DW1000NgRTLS::transmitFinalMessage(
                    &cont_recv[7],
                    replyDelayUs,
                    DW1000Ng::getTransmitTimestamp(), // Poll transmit time
                    DW1000Ng::getReceiveTimestamp()  // Response to poll receive time
            );
        } else {
            return {false, false, 0, 0};
        }

        if(!DW1000NgRTLS::waitForNextRangingStep()) return {false, false, 0, 0};

        size_t act_len = DW1000Ng::getReceivedDataLength();
        byte act_recv[act_len];
        DW1000Ng::getReceivedData(act_recv, act_len);

        if(act_len > 10 && act_recv[9] == ACTIVITY_CONTROL) {
            if (act_len > 12 && act_recv[10] == RANGING_CONFIRM) {
                return {true, true, DW1000NgUtils::bytesAsValue(&act_recv[11], 2), 0};
            } else if(act_len > 12 && act_recv[10] == ACTIVITY_FINISHED) {
                return {true, false, 0, calculateNewBlinkRate(act_recv)};
            }
        } else {
            return {false, false, 0, 0};
        }
    }

    RangeInfrastructureResult tagRangeInfrastructure(uint16_t target_anchor, uint16_t finalMessageDelay) {
        RangeResult result = tagFinishRange(target_anchor, finalMessageDelay);
        if(!result.success) return {false , 0};

        while(result.success && result.next) {
            result = tagFinishRange(result.next_anchor, finalMessageDelay);
            if(!result.success) return {false , 0};

        }

        if(result.success && result.new_blink_rate != 0) {
            return { true, result.new_blink_rate };
        } else {
            if(!result.success)
                return { false , 0 };
        }
    }

    RangeInfrastructureResult tagTwrLocalize(uint16_t finalMessageDelay) {
        RangeRequestResult request_result = DW1000NgRTLS::tagRangeRequest();

        if(request_result.success) {

            RangeInfrastructureResult result = DW1000NgRTLS::tagRangeInfrastructure(request_result.target_anchor, finalMessageDelay);

            if(result.success)
                return result;
        }
        return {false, 0};
    }
}