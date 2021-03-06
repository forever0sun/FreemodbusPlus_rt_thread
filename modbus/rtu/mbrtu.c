/* 
 * FreeModbus Libary: A portable Modbus implementation for Modbus ASCII/RTU.
 * Copyright (c) 2006 Christian Walter <wolti@sil.at>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * File: $Id: mbrtu.c,v 1.18 2007/09/12 10:15:56 wolti Exp $
 */

/* ----------------------- System includes ----------------------------------*/
#include "stdlib.h"
#include "string.h"
#include "board.h"

/* ----------------------- Platform includes --------------------------------*/
#include "port.h"

/* ----------------------- Modbus includes ----------------------------------*/
#include "mb.h"
#include "mbrtu.h"
#include "mbframe.h"

#include "mbcrc.h"
#include "mbport.h"

/* ----------------------- Defines ------------------------------------------*/
#define MB_SER_PDU_SIZE_MIN     4       /*!< Minimum size of a Modbus RTU frame. */
//修改每帧最大字节数 256->512
#define MB_SER_PDU_SIZE_MAX     512     /*!< Maximum size of a Modbus RTU frame. */
#define MB_SER_PDU_SIZE_CRC     2       /*!< Size of CRC field in PDU. */
#define MB_SER_PDU_ADDR_OFF     0       /*!< Offset of slave address in Ser-PDU. */
#define MB_SER_PDU_PDU_OFF      1       /*!< Offset of Modbus-PDU in Ser-PDU. */

/* ----------------------- Start implementation -----------------------------*/
eMBErrorCode
eMBRTUInit( eMBControlBlockSlave* peConBlock, UCHAR ucSlaveAddress, UCHAR ucPort, ULONG ulBaudRate, eMBParity eParity )
{
    eMBErrorCode    eStatus = MB_ENOERR;
    ULONG           usTimerT35_50us;

    ( void )ucSlaveAddress;
    
    /* malloc the ucBuf to store the rtu send/recv data */
    peConBlock->ucBuf = rt_malloc(MB_SER_PDU_SIZE_MAX);
    if(peConBlock->ucBuf == NULL){
        eStatus = MB_EPORTERR;
        goto end;
    }
    
    ENTER_CRITICAL_SECTION(  );

    /* Modbus RTU uses 8 Databits. */
    if( peConBlock->port.type.portRTU.pPortSerialInit( &peConBlock->port, ucPort, ulBaudRate, 8, eParity ) != TRUE ){
        eStatus = MB_EPORTERR;
        rt_free((void*)peConBlock->ucBuf);
    }
    else{
        /* If baudrate > 19200 then we should use the fixed timer values
         * t35 = 1750us. Otherwise t35 must be 3.5 times the character time.
         */
        if( ulBaudRate > 19200 ){
            usTimerT35_50us = 35;       /* 1800us. */
        }
        else{
            /* The timer reload value for a character is given by:
             *
             * ChTimeValue = Ticks_per_1s / ( Baudrate / 11 )
             *             = 11 * Ticks_per_1s / Baudrate
             *             = 220000 / Baudrate
             * The reload for t3.5 is 1.5 times this value and similary
             * for t3.5.
             */
            usTimerT35_50us = ( 7UL * 220000UL ) / ( 2UL * ulBaudRate );
        }
        if( peConBlock->port.type.portRTU.pPortTimersInit( &peConBlock->port, ( USHORT ) usTimerT35_50us ) != TRUE ){
            eStatus = MB_EPORTERR;
            rt_free((void*)peConBlock->ucBuf);
        }
    }
    EXIT_CRITICAL_SECTION(  );
    
    end:
    return eStatus;
}

void
eMBRTUStart( eMBControlBlockSlave* peConBlock )
{
    ENTER_CRITICAL_SECTION(  );
    /* Initially the receiver is in the state STATE_RX_INIT. we start
     * the timer and if no character is received within t3.5 we change
     * to STATE_RX_IDLE. This makes sure that we delay startup of the
     * modbus protocol stack until the bus is free.
     */
    peConBlock->eRcvState = MB_STATE_RX_INIT;
    peConBlock->port.type.portRTU.pPortSerialEnable( &peConBlock->port, TRUE, FALSE );
    peConBlock->port.type.portRTU.pPortTimersEnable( &peConBlock->port );

    EXIT_CRITICAL_SECTION(  );
}

void
eMBRTUStop( eMBControlBlockSlave* peConBlock ){
    ENTER_CRITICAL_SECTION(  );
    peConBlock->port.type.portRTU.pPortSerialEnable( &peConBlock->port, FALSE, FALSE );
    peConBlock->port.type.portRTU.pPortTimersDisable( &peConBlock->port );
    EXIT_CRITICAL_SECTION(  );
}

eMBErrorCode
eMBRTUReceive( eMBControlBlockSlave* peConBlock, UCHAR * pucRcvAddress, UCHAR ** pucFrame, USHORT * pusLength )
{
    eMBErrorCode    eStatus = MB_ENOERR;

    ENTER_CRITICAL_SECTION(  );
    RT_ASSERT( peConBlock->usRcvBufferPos < MB_SER_PDU_SIZE_MAX );

    /* Length and CRC check */
    if( ( peConBlock->usRcvBufferPos >= MB_SER_PDU_SIZE_MIN )
        && ( usMBCRC16( ( UCHAR * ) peConBlock->ucBuf, peConBlock->usRcvBufferPos ) == 0 ) )
    {
        /* Save the address field. All frames are passed to the upper layed
         * and the decision if a frame is used is done there.
         */
        *pucRcvAddress = peConBlock->ucBuf[MB_SER_PDU_ADDR_OFF];

        /* Total length of Modbus-PDU is Modbus-Serial-Line-PDU minus
         * size of address field and CRC checksum.
         */
        *pusLength = ( USHORT )( peConBlock->usRcvBufferPos - MB_SER_PDU_PDU_OFF - MB_SER_PDU_SIZE_CRC );

        /* Return the start of the Modbus PDU to the caller. */
        *pucFrame = ( UCHAR * ) & peConBlock->ucBuf[MB_SER_PDU_PDU_OFF];
    }
    else
    {
        eStatus = MB_EIO;
    }

    EXIT_CRITICAL_SECTION(  );
    return eStatus;
}

eMBErrorCode
eMBRTUSend( eMBControlBlockSlave* peConBlock, UCHAR ucSlaveAddress, const UCHAR * pucFrame, USHORT usLength )
{
    eMBErrorCode    eStatus = MB_ENOERR;
    USHORT          usCRC16;

    ENTER_CRITICAL_SECTION(  );

    /* Check if the receiver is still in idle state. If not we where to
     * slow with processing the received frame and the master sent another
     * frame on the network. We have to abort sending the frame.
     */
    if( peConBlock->eRcvState == MB_STATE_RX_IDLE )
    {
        /* First byte before the Modbus-PDU is the slave address. */
        peConBlock->pucSndBufferCur = ( UCHAR * ) pucFrame - 1;
        peConBlock->usSndBufferCount = 1;

        /* Now copy the Modbus-PDU into the Modbus-Serial-Line-PDU. */
        peConBlock->pucSndBufferCur[MB_SER_PDU_ADDR_OFF] = ucSlaveAddress;
        peConBlock->usSndBufferCount += usLength;

        /* Calculate CRC16 checksum for Modbus-Serial-Line-PDU. */
        usCRC16 = usMBCRC16( ( UCHAR * ) peConBlock->pucSndBufferCur, peConBlock->usSndBufferCount );
        peConBlock->ucBuf[peConBlock->usSndBufferCount++] = ( UCHAR )( usCRC16 & 0xFF );
        peConBlock->ucBuf[peConBlock->usSndBufferCount++] = ( UCHAR )( usCRC16 >> 8 );

        /* Activate the transmitter. */
        peConBlock->eSndState = MB_STATE_TX_XMIT;
        peConBlock->port.type.portRTU.pPortSerialEnable( &peConBlock->port, FALSE, TRUE );
    }
    else
    {
        eStatus = MB_EIO;
    }
    EXIT_CRITICAL_SECTION(  );
    return eStatus;
}

BOOL
xMBRTUReceiveFSM( eMBControlBlockSlave* peConBlock )
{
    BOOL            xTaskNeedSwitch = FALSE;
    UCHAR           ucByte;

    RT_ASSERT( peConBlock->eSndState == MB_STATE_TX_IDLE );

    /* Always read the character. */
    ( void )peConBlock->port.type.portRTU.pPortSerialGetByte( &peConBlock->port, ( CHAR * ) & ucByte );

    switch ( peConBlock->eRcvState )
    {
        /* If we have received a character in the init state we have to
         * wait until the frame is finished.
         */
    case MB_STATE_RX_INIT:
        peConBlock->port.type.portRTU.pPortTimersEnable( &peConBlock->port );
        break;

        /* In the error state we wait until all characters in the
         * damaged frame are transmitted.
         */
    case MB_STATE_RX_ERROR:
        peConBlock->port.type.portRTU.pPortTimersEnable( &peConBlock->port );
        break;

        /* In the idle state we wait for a new character. If a character
         * is received the t1.5 and t3.5 timers are started and the
         * receiver is in the state STATE_RX_RECEIVCE.
         */
    case MB_STATE_RX_IDLE:
        peConBlock->usRcvBufferPos = 0;
        peConBlock->ucBuf[peConBlock->usRcvBufferPos++] = ucByte;
        peConBlock->eRcvState = MB_STATE_RX_RCV;

        /* Enable t3.5 timers. */
        peConBlock->port.type.portRTU.pPortTimersEnable( &peConBlock->port );
        break;

        /* We are currently receiving a frame. Reset the timer after
         * every character received. If more than the maximum possible
         * number of bytes in a modbus frame is received the frame is
         * ignored.
         */
    case MB_STATE_RX_RCV:
        if( peConBlock->usRcvBufferPos < MB_SER_PDU_SIZE_MAX )
        {
            peConBlock->ucBuf[peConBlock->usRcvBufferPos++] = ucByte;
        }
        else
        {
            peConBlock->eRcvState = MB_STATE_RX_ERROR;
        }
        peConBlock->port.type.portRTU.pPortTimersEnable( &peConBlock->port );
        break;
    }
    return xTaskNeedSwitch;
}


BOOL
xMBRTUTransmitFSM( eMBControlBlockSlave* peConBlock )
{
    BOOL            xNeedPoll = FALSE;

    RT_ASSERT( peConBlock->eRcvState == MB_STATE_RX_IDLE );

    switch ( peConBlock->eSndState )
    {
        /* We should not get a transmitter event if the transmitter is in
         * idle state.  */
    case MB_STATE_TX_IDLE:
        /* enable receiver/disable transmitter. */
        peConBlock->port.type.portRTU.pPortSerialEnable( &peConBlock->port, TRUE, FALSE );
        break;

    case MB_STATE_TX_XMIT:
        /* check if we are finished. */
        if( peConBlock->usSndBufferCount != 0 ){
            peConBlock->port.type.portRTU.pPortSerialPutByte( &peConBlock->port, ( CHAR )*peConBlock->pucSndBufferCur );
            peConBlock->pucSndBufferCur++;  /* next byte in sendbuffer. */
            peConBlock->usSndBufferCount--;
        }
        else{
            xNeedPoll = peConBlock->port.pPortEventPost( &peConBlock->port, EV_FRAME_SENT );
            /* Disable transmitter. This prevents another transmit buffer
             * empty interrupt. */
            peConBlock->port.type.portRTU.pPortSerialWaitSendOver( &peConBlock->port );
            peConBlock->port.type.portRTU.pPortSerialEnable( &peConBlock->port, TRUE, FALSE );
            peConBlock->eSndState = MB_STATE_TX_IDLE;
        }
        break;
    }

    return xNeedPoll;
}

BOOL
xMBRTUTimerT35Expired( eMBControlBlockSlave* peConBlock )
{
    BOOL xNeedPoll = FALSE;

    switch ( peConBlock->eRcvState )
    {
        /* Timer t35 expired. Startup phase is finished. */
    case MB_STATE_RX_INIT:
        xNeedPoll = peConBlock->port.pPortEventPost( &peConBlock->port, EV_READY );
        break;

        /* A frame was received and t35 expired. Notify the listener that
         * a new frame was received. */
    case MB_STATE_RX_RCV:
        xNeedPoll = peConBlock->port.pPortEventPost( &peConBlock->port, EV_FRAME_RECEIVED );
        break;

        /* An error occured while receiving the frame. */
    case MB_STATE_RX_ERROR:
        break;

        /* Function called in an illegal state. */
    default:
        RT_ASSERT( ( peConBlock->eRcvState == MB_STATE_RX_INIT ) ||
                ( peConBlock->eRcvState == MB_STATE_RX_RCV ) || ( peConBlock->eRcvState == MB_STATE_RX_ERROR ) );
         break;
    }

    peConBlock->port.type.portRTU.pPortTimersDisable( &peConBlock->port );
    peConBlock->eRcvState = MB_STATE_RX_IDLE;

    return xNeedPoll;
}
