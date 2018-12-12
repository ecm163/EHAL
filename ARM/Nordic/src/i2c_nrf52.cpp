/*--------------------------------------------------------------------------
File   : i2c_nrf52.cpp

Author : Hoang Nguyen Hoan          Oct. 12, 2016

Desc   : I2C implementation on nRF52 series MCU using EasyDMA

Copyright (c) 2016, I-SYST inc., all rights reserved

Permission to use, copy, modify, and distribute this software for any purpose
with or without fee is hereby granted, provided that the above copyright
notice and this permission notice appear in all copies, and none of the
names : I-SYST or its contributors may be used to endorse or
promote products derived from this software without specific prior written
permission.

For info or contributing contact : hnhoan at i-syst dot com

THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------------
Modified by         Date            Description

----------------------------------------------------------------------------*/
#include "nrf.h"

#include "coredev/i2c.h"
#include "iopinctrl.h"
#include "idelay.h"
#include "i2c_spi_nrf52_irq.h"

#define NRF52_I2C_MAXDEV        2
#define NRF52_I2C_MAXSLAVE		2

#define NRF52_I2C_DMA_MAXCNT    255

#define NRF52_I2C_TRBUFF_SIZE	4

#pragma pack(push, 4)
typedef struct {
	int DevNo;
	I2CDEV *pI2cDev;
	uint32_t Clk;
	union {
		NRF_TWIM_Type *pDmaReg;	// Register map
		NRF_TWI_Type *pReg;		// Register map
		NRF_TWIS_Type *pDmaSReg;// Register map
	};
	uint8_t TRData[NRF52_I2C_MAXSLAVE][NRF52_I2C_TRBUFF_SIZE];
} NRF52_I2CDEV;
#pragma pack(pop)

static NRF52_I2CDEV s_nRF52I2CDev[NRF52_I2C_MAXDEV] = {
	{
		0, NULL, 0, (NRF_TWIM_Type *)NRF_TWIM0_BASE
	},
	{
		1, NULL, 0, (NRF_TWIM_Type *)NRF_TWIM1_BASE
	},
};

bool nRF52I2CWaitStop(NRF52_I2CDEV * const pDev, int Timeout)
{
    do {
        if (pDev->pReg->EVENTS_ERROR)
        {
            // Abort in case error
            pDev->pReg->ERRORSRC = pDev->pReg->ERRORSRC;
            pDev->pReg->EVENTS_ERROR = 0;
            pDev->pReg->TASKS_RESUME = 1;
            pDev->pReg->TASKS_STOP = 1;
            while( !pDev->pReg->EVENTS_STOPPED );

            return false;
        }
        if (pDev->pReg->EVENTS_STOPPED)
        {
            // Must wait for stop, other wise DMA count would
            // not be updated with correct value
            pDev->pReg->EVENTS_STOPPED = 0;
            pDev->pDmaReg->EVENTS_TXSTARTED = 0;
            pDev->pDmaReg->EVENTS_RXSTARTED = 0;
            return true;
        }
    } while (Timeout-- >  0);

    return false;
}

bool nRF52I2CWaitRxComplete(NRF52_I2CDEV * const pDev, int Timeout)
{
    do {
        if (pDev->pReg->EVENTS_ERROR)
        {
            while ( !nRF52I2CWaitStop( pDev, Timeout ) );

            return false;
        }

        if (pDev->pI2cDev->bDmaEn)
        {
			if (pDev->pDmaReg->EVENTS_LASTRX)
			{
				// Must wait for last DMA then issue a stop
				pDev->pDmaReg->EVENTS_LASTRX = 0;

				return true;
			}
        }
        else
        {
            if (pDev->pReg->EVENTS_RXDREADY)
            {
                pDev->pReg->EVENTS_RXDREADY = 0;

                return true;
            }
        }
    } while (Timeout-- >  0);

    return false;
}

bool nRF52I2CWaitTxComplete(NRF52_I2CDEV * const pDev, int Timeout)
{
    do {
        if (pDev->pReg->EVENTS_ERROR)
        {
            while ( !nRF52I2CWaitStop( pDev, Timeout ) );

            return false;
        }
        if (pDev->pI2cDev->bDmaEn)
        {
			if (pDev->pDmaReg->EVENTS_LASTTX)
			{
				// Must wait for last DMA then issue a stop
				pDev->pDmaReg->EVENTS_LASTTX = 0;

				return true;
			}
        }
        else
        {
            if (pDev->pReg->EVENTS_TXDSENT)
            {
                pDev->pReg->EVENTS_TXDSENT = 0;

                return true;
            }
        }
    } while (Timeout-- >  0);

    return false;
}

void nRF52I2CDisable(DEVINTRF * const pDev)
{
	NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;

	dev->pReg->ENABLE = (TWIM_ENABLE_ENABLE_Disabled << TWIM_ENABLE_ENABLE_Pos);
}

void nRF52I2CEnable(DEVINTRF * const pDev)
{
	NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;

    if (dev->pI2cDev->bDmaEn)
    {
		if (dev->pI2cDev->Mode == I2CMODE_SLAVE)
		{
			dev->pReg->ENABLE = (TWIS_ENABLE_ENABLE_Enabled << TWIS_ENABLE_ENABLE_Pos);
		}
		else
		{
			dev->pReg->ENABLE = (TWIM_ENABLE_ENABLE_Enabled << TWIM_ENABLE_ENABLE_Pos);
		}
    }
    else
    {
    	dev->pReg->ENABLE = (TWI_ENABLE_ENABLE_Enabled << TWI_ENABLE_ENABLE_Pos);
    }
}

void nRF52I2CPowerOff(DEVINTRF * const pDev)
{
	NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;

	// Undocumented Power down I2C.  Nordic Bug with DMA causing high current consumption
	*(volatile uint32_t *)((uint32_t)dev->pReg + 0xFFC);
	*(volatile uint32_t *)((uint32_t)dev->pReg + 0xFFC) = 1;
	*(volatile uint32_t *)((uint32_t)dev->pReg + 0xFFC) = 0;
}

int nRF52I2CGetRate(DEVINTRF * const pDev)
{
	NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;

	return dev->pI2cDev->Rate;
}

int nRF52I2CSetRate(DEVINTRF * const pDev, int RateHz)
{
	NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;

	if (RateHz < 250000)
	{
		dev->pReg->FREQUENCY = TWIM_FREQUENCY_FREQUENCY_K100;
		dev->pI2cDev->Rate = 100000;
	}
	else if (RateHz < 400000)
	{
		dev->pReg->FREQUENCY = TWIM_FREQUENCY_FREQUENCY_K250;
		dev->pI2cDev->Rate = 250000;
	}
	else
	{
		dev->pReg->FREQUENCY = TWIM_FREQUENCY_FREQUENCY_K400;
		dev->pI2cDev->Rate = 400000;
	}

	return dev->pI2cDev->Rate;
}

bool nRF52I2CStartRx(DEVINTRF * const pDev, int DevAddr)
{
	NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;

	dev->pReg->ADDRESS = DevAddr;
	dev->pReg->INTENCLR = 0xFFFFFFFF;
	return true;
}

// Receive Data only, no Start/Stop condition
int nRF52I2CRxDataDMA(DEVINTRF * const pDev, uint8_t *pBuff, int BuffLen)
{
	NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;
	uint32_t d;
	int cnt = 0;

	while (BuffLen > 0)
	{
		int l = min(BuffLen, NRF52_I2C_DMA_MAXCNT);
		dev->pReg->EVENTS_ERROR = 0;
		dev->pReg->EVENTS_STOPPED = 0;
		dev->pDmaReg->RXD.PTR = (uint32_t)pBuff;
		dev->pDmaReg->RXD.MAXCNT = l;
		dev->pDmaReg->RXD.LIST = 0;
		dev->pReg->SHORTS = TWIM_SHORTS_LASTRX_STOP_Msk;
		dev->pReg->EVENTS_SUSPENDED = 0;
		dev->pReg->TASKS_RESUME = 1;
		dev->pReg->TASKS_STARTRX = 1;

		if (nRF52I2CWaitRxComplete(dev, 1000000) == false)
		    break;

		BuffLen -= l;
		pBuff += l;
		cnt += l;
	}
	return cnt;
}

// Receive Data only, no Start/Stop condition
int nRF5xI2CRxData(DEVINTRF *pDev, uint8_t *pBuff, int BuffLen)
{
	NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;
	int cnt = 0;

	if (pBuff == NULL || BuffLen <= 0)
	{
		return 0;
	}

	dev->pReg->EVENTS_STOPPED = 0;
	dev->pReg->TASKS_STARTRX = 1;

	while (BuffLen > 0)
	{
		if (nRF52I2CWaitRxComplete(dev, 100000) == false)
		{
			break;
		}

		*pBuff = dev->pReg->RXD;

		BuffLen--;
		pBuff++;
		cnt++;
	}

	return cnt;
}

void nRF52I2CStopRx(DEVINTRF * const pDev)
{
    NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;
    dev->pReg->TASKS_RESUME = 1;
    dev->pReg->TASKS_STOP = 1;

    if (dev->pI2cDev->bDmaEn == false)
    {
        // must read dummy last byte to generate NACK & STOP condition
    	nRF52I2CWaitRxComplete(dev, 100000);
    	uint8_t d __attribute__((unused)) = dev->pReg->RXD;
    }
    nRF52I2CWaitStop(dev, 1000);
}

bool nRF52I2CStartTx(DEVINTRF * const pDev, int DevAddr)
{
	NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;

	dev->pReg->ADDRESS = DevAddr;
	dev->pReg->INTENCLR = 0xFFFFFFFF;

	return true;
}

// Send Data only, no Start/Stop condition
int nRF52I2CTxDataDMA(DEVINTRF * const pDev, uint8_t *pData, int DataLen)
{
	NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;
	uint32_t d;
	int cnt = 0;

	while (DataLen > 0)
	{
		int l = min(DataLen, NRF52_I2C_DMA_MAXCNT);

		dev->pReg->EVENTS_ERROR = 0;
		dev->pReg->EVENTS_STOPPED = 0;
	    if (dev->pI2cDev->bDmaEn)
	    {
			dev->pDmaReg->TXD.PTR = (uint32_t)pData;
			dev->pDmaReg->TXD.MAXCNT = l;
			dev->pDmaReg->TXD.LIST = 0;
	    }
		dev->pReg->SHORTS = (TWIM_SHORTS_LASTTX_SUSPEND_Enabled << TWIM_SHORTS_LASTTX_SUSPEND_Pos);
		dev->pReg->EVENTS_SUSPENDED = 0;
		dev->pReg->TASKS_RESUME = 1;
		dev->pReg->TASKS_STARTTX = 1;

		if (nRF52I2CWaitTxComplete(dev, 100000) == false)
            break;

		DataLen -= l;
		pData += l;
		cnt += l;
	}
	return cnt;
}

int nRF52I2CTxData(DEVINTRF * const pDev, uint8_t *pData, int DataLen)
{
	NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;
	uint32_t d;
	int cnt = 0;

	dev->pReg->TASKS_STARTTX = 1;

	while (DataLen > 0)
	{
		dev->pReg->TXD = *pData;

		if (nRF52I2CWaitTxComplete(dev, 10000) == false)
		{
			break;
		}

		DataLen--;
		pData++;
		cnt++;
	}
	return cnt;
}

void nRF52I2CStopTx(DEVINTRF * const pDev)
{
    NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;

    if (dev->pI2cDev->bDmaEn)
    {
		if (dev->pDmaReg->EVENTS_LASTTX == 1)
		{
			dev->pDmaReg->EVENTS_LASTTX = 0;
		}
    }
	dev->pReg->EVENTS_SUSPENDED = 0;
	dev->pReg->TASKS_RESUME = 1;
    dev->pReg->TASKS_STOP = 1;
    nRF52I2CWaitStop(dev, 1000);
}

void nRF52I2CReset(DEVINTRF * const pDev)
{
    NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;

    nRF52I2CDisable(pDev);

    IOPinConfig(0, dev->pReg->PSELSCL, 0, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL);
    IOPinConfig(0, dev->pReg->PSELSDA, 0, IOPINDIR_INPUT, IOPINRES_NONE, IOPINTYPE_NORMAL);

    IOPinSet(0, dev->pReg->PSELSDA);

    for (int i = 0; i < 10; i++)
    {
        IOPinSet(0, dev->pReg->PSELSCL);
        usDelay(5);
        IOPinClear(0, dev->pReg->PSELSCL);
        usDelay(5);
    }
    IOPinConfig(0, dev->pReg->PSELSDA, 0, IOPINDIR_OUTPUT, IOPINRES_NONE, IOPINTYPE_NORMAL);
    IOPinClear(0, dev->pReg->PSELSDA);
    usDelay(5);
    IOPinSet(0, dev->pReg->PSELSCL);
    usDelay(2);
    IOPinSet(0, dev->pReg->PSELSDA);

    nRF52I2CEnable(pDev);
}

void I2CIrqHandler(int DevNo, DEVINTRF * const pDev)
{
    NRF52_I2CDEV *dev = (NRF52_I2CDEV*)pDev->pDevData;

    if (dev->pI2cDev->Mode == I2CMODE_SLAVE)
    {
    	// Slave mode
    	if (dev->pDmaSReg->EVENTS_READ)
    	{
    		// Read command received

    		if (pDev->EvtCB)
    		{
    			int len = dev->pDmaSReg->EVENTS_RXSTARTED ? dev->pDmaSReg->RXD.AMOUNT : 0;

    			pDev->EvtCB(pDev, DEVINTRF_EVT_READ_RQST, NULL, len);
    		}
    		dev->pDmaSReg->EVENTS_RXSTARTED = 0;
    		dev->pDmaSReg->EVENTS_READ = 0;
    		dev->pDmaSReg->TXD.PTR = (uint32_t)dev->pI2cDev->pRRData[dev->pDmaSReg->MATCH];
    		dev->pDmaSReg->TXD.MAXCNT = dev->pI2cDev->RRDataLen[dev->pDmaSReg->MATCH] & 0xFF;
    		dev->pDmaSReg->TASKS_PREPARETX = 1;
    		dev->pDmaSReg->TASKS_RESUME = 1;
    	}

    	if (dev->pDmaSReg->EVENTS_WRITE)
    	{
    		// Write command received

    		if (pDev->EvtCB)
    		{
    			pDev->EvtCB(pDev, DEVINTRF_EVT_WRITE_RQST, NULL, 0);
    		}
    		dev->pDmaSReg->EVENTS_WRITE = 0;
    		dev->pDmaSReg->RXD.PTR = (uint32_t)dev->pI2cDev->pTRBuff[dev->pDmaSReg->MATCH];
    		dev->pDmaSReg->RXD.MAXCNT = dev->pI2cDev->TRBuffLen[dev->pDmaSReg->MATCH];
    		dev->pDmaSReg->SHORTS = 0;
    		dev->pDmaSReg->TASKS_PREPARERX = 1;
    		dev->pDmaSReg->TASKS_RESUME = 1;
    	}

    	if (dev->pDmaSReg->EVENTS_STOPPED)
    	{
    		int len = 0;

    		if (dev->pDmaSReg->EVENTS_RXSTARTED)
    		{
    			len = dev->pDmaSReg->RXD.AMOUNT;
    			dev->pDmaSReg->EVENTS_RXSTARTED = 0;
    		}
    		if (dev->pDmaSReg->EVENTS_TXSTARTED)
    		{
    			len = dev->pDmaSReg->TXD.AMOUNT;
    			dev->pDmaSReg->EVENTS_TXSTARTED = 0;
    		}
    		dev->pDmaSReg->EVENTS_STOPPED = 0;
    		if (pDev->EvtCB)
    		{
    			pDev->EvtCB(pDev, DEVINTRF_EVT_COMPLETED, NULL, len);
    		}
    	}
    	if (dev->pDmaSReg->EVENTS_ERROR)
    	{
    		dev->pDmaSReg->EVENTS_ERROR = 0;
    	}
    }
    else
    {
    	// Master mode
    	// TODO: implement interrupt handling for master mode
    }

}

bool I2CInit(I2CDEV * const pDev, const I2CCFG *pCfgData)
{
	if (pDev == NULL || pCfgData == NULL)
	{
		return false;
	}

	if (pCfgData->DevNo < 0 || pCfgData->DevNo > 2)
	{
		return false;
	}

	// Get the correct register map
	NRF_TWIM_Type *reg = s_nRF52I2CDev[pCfgData->DevNo].pDmaReg;

	memcpy(pDev->Pins, pCfgData->Pins, sizeof(IOPINCFG) * I2C_MAX_NB_IOPIN);

	// Configure I/O pins
	IOPinCfg(pCfgData->Pins, I2C_MAX_NB_IOPIN);
    IOPinSet(pCfgData->Pins[I2C_SDA_IOPIN_IDX].PortNo, pCfgData->Pins[I2C_SDA_IOPIN_IDX].PinNo);
    IOPinSet(pCfgData->Pins[I2C_SCL_IOPIN_IDX].PortNo, pCfgData->Pins[I2C_SCL_IOPIN_IDX].PinNo);

    reg->PSEL.SCL = pCfgData->Pins[I2C_SCL_IOPIN_IDX].PinNo;
    reg->PSEL.SDA = pCfgData->Pins[I2C_SDA_IOPIN_IDX].PinNo;

    //pDev->DevIntrf.MaxRetry = pCfgData->MaxRetry;
    pDev->Mode = pCfgData->Mode;

	s_nRF52I2CDev[pCfgData->DevNo].pI2cDev  = pDev;
	pDev->DevIntrf.pDevData = (void*)&s_nRF52I2CDev[pCfgData->DevNo];

	nRF52I2CSetRate(&pDev->DevIntrf, pCfgData->Rate);

	pDev->DevIntrf.Type = DEVINTRF_TYPE_I2C;
	pDev->bDmaEn = pCfgData->bDmaEn;
	pDev->DevIntrf.Disable = nRF52I2CDisable;
	pDev->DevIntrf.Enable = nRF52I2CEnable;
	pDev->DevIntrf.PowerOff = nRF52I2CPowerOff;
	pDev->DevIntrf.GetRate = nRF52I2CGetRate;
	pDev->DevIntrf.SetRate = nRF52I2CSetRate;
	pDev->DevIntrf.StartRx = nRF52I2CStartRx;
	pDev->DevIntrf.StopRx = nRF52I2CStopRx;
	pDev->DevIntrf.StartTx = nRF52I2CStartTx;

	if (pDev->bDmaEn)
	{
		pDev->DevIntrf.RxData = nRF52I2CRxDataDMA;
		pDev->DevIntrf.TxData = nRF52I2CTxDataDMA;
	}
	else
	{
		pDev->DevIntrf.RxData = nRF5xI2CRxData;
		pDev->DevIntrf.TxData = nRF52I2CTxData;
	}
	pDev->DevIntrf.StopTx = nRF52I2CStopTx;
	pDev->DevIntrf.Reset = nRF52I2CReset;
	pDev->DevIntrf.IntPrio = pCfgData->IntPrio;
	pDev->DevIntrf.EvtCB = pCfgData->EvtCB;
	pDev->DevIntrf.bBusy = false;
	pDev->DevIntrf.MaxRetry = pCfgData->MaxRetry;

	reg->SHORTS = 0;

	// Clear all errors
    if (reg->EVENTS_ERROR)
    {
        reg->ERRORSRC = reg->ERRORSRC;
        reg->EVENTS_ERROR = 0;
        reg->TASKS_RESUME = 1;
        reg->TASKS_STOP = 1;
    }

    usDelay(1000);

    reg->EVENTS_LASTRX = 0;
    reg->EVENTS_LASTTX = 0;
    reg->EVENTS_RXSTARTED = 0;
    reg->EVENTS_TXSTARTED = 0;
    reg->EVENTS_SUSPENDED = 0;
    reg->EVENTS_STOPPED = 0;

    uint32_t enval = (TWI_ENABLE_ENABLE_Enabled << TWI_ENABLE_ENABLE_Pos);
    uint32_t inten = 0;

    if (pDev->bDmaEn)
    {
    	enval = (TWIM_ENABLE_ENABLE_Enabled << TWIM_ENABLE_ENABLE_Pos);
    }

    if (pCfgData->Mode == I2CMODE_SLAVE)
    {
    	NRF_TWIS_Type *sreg = s_nRF52I2CDev[pCfgData->DevNo].pDmaSReg;
        pDev->NbSlaveAddr = min(pCfgData->NbSlaveAddr, NRF52_I2C_MAXSLAVE);

        sreg->CONFIG = 0;
        sreg->ORC = 0xff;

        for (int i = 0; i < pDev->NbSlaveAddr; i++)
        {
        	pDev->SlaveAddr[i] = pCfgData->SlaveAddr[i];
        	sreg->ADDRESS[i] = (uint32_t)pCfgData->SlaveAddr[i];
        	if (pDev->SlaveAddr[i] != 0)
        	{
        		sreg->CONFIG |= 1<<i;
        	}
        }

		sreg->SHORTS = TWIS_SHORTS_READ_SUSPEND_Msk | TWIS_SHORTS_WRITE_SUSPEND_Msk;
        sreg->EVENTS_READ = 0;
        sreg->EVENTS_WRITE = 0;
        enval = TWIS_ENABLE_ENABLE_Enabled << TWIS_ENABLE_ENABLE_Pos;
        inten = (TWIS_INTEN_READ_Enabled << TWIS_INTEN_READ_Pos) | (TWIS_INTEN_WRITE_Enabled << TWIS_INTEN_WRITE_Pos) |
        		(TWIS_INTEN_STOPPED_Enabled << TWIS_INTEN_STOPPED_Pos);
    }

    if (pCfgData->bIntEn)
    {
    	SetI2cSpiIntHandler(pCfgData->DevNo, &pDev->DevIntrf, I2CIrqHandler);

    	if (pCfgData->DevNo == 0)
    	{
    		NVIC_ClearPendingIRQ(SPIM0_SPIS0_TWIM0_TWIS0_SPI0_TWI0_IRQn);
    		NVIC_SetPriority(SPIM0_SPIS0_TWIM0_TWIS0_SPI0_TWI0_IRQn, pCfgData->IntPrio);
    		NVIC_EnableIRQ(SPIM0_SPIS0_TWIM0_TWIS0_SPI0_TWI0_IRQn);
    	}
    	else
    	{
    		NVIC_ClearPendingIRQ(SPIM1_SPIS1_TWIM1_TWIS1_SPI1_TWI1_IRQn);
    		NVIC_SetPriority(SPIM1_SPIS1_TWIM1_TWIS1_SPI1_TWI1_IRQn, pCfgData->IntPrio);
    		NVIC_EnableIRQ(SPIM1_SPIS1_TWIM1_TWIS1_SPI1_TWI1_IRQn);
    	}

    	reg->INTEN = inten;
    }
	reg->ENABLE = enval;

	return true;
}
