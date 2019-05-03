
#include "stp.h"
#include "drivers/LPC23xx_enet.h"
#include "drivers/timer.h"
#include "drivers/scheduler.h"
#include "debug_leds.h"
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define PORT_RJ45_1		0
#define PORT_RJ45_2		1
#define PORT_RMII		2

static void StpCallback_EnableBpduTrapping (const struct STP_BRIDGE* bridge, bool enable, unsigned int timestamp)
{
	if (enable)
	{
		// Enable special tagging for RX and TX.
		// See the Miscellaneous Control Register, page 102 of the IP175D datasheet.
		unsigned short reg = ENET_MIIReadRegister (21, 22);
		reg = reg | 3;
		ENET_MIIWriteRegister (21, 22, reg);

		// Add source-port tag for packets going to port 5 (RMII port).
		// See the Add Tag Control Register, page 111 of the IP175D datasheet.
		ENET_MIIWriteRegister (23, 8, (1 << 5));

		// Remove source-port tag from packets going to ports 0-4.
		// See the Remove Tag Control Register, page 111 of the IP175D datasheet.
		ENET_MIIWriteRegister (23, 16, 0x1f);

		// Forward BPDUs only to the CPU.
		// Page 79 of the IP175D datasheet.
		reg = ENET_MIIReadRegister (20, 8);
		reg = (reg & ~3u) | 1u;
		ENET_MIIWriteRegister (20, 8, reg);
	}
	else
	{
		// Here goes the code that undoes the switch chip configuration from above.
		// This is not yet implemented in this demo app.
	}
}

static void StpCallback_EnableLearning (const struct STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex, bool enable, unsigned int timestamp)
{
	unsigned int i = ENET_MIIReadRegister (20, 6);

	if (portIndex == PORT_RJ45_1)
	{
		i = (i & ~(1ul << 0)) | ((enable != 0) ? (1ul << 0) : 0);
	}
	else if (portIndex == PORT_RJ45_2)
	{
		i = (i & ~(1ul << 1)) | ((enable != 0) ? (1ul << 1) : 0);
	}
	else if (portIndex == PORT_RMII)
	{
		i = (i & ~(1ul << 5)) | ((enable != 0) ? (1ul << 5) : 0);
	}
	else
		assert (0);

	ENET_MIIWriteRegister (20, 6, i);

	update_debug_leds(bridge);
}

static void StpCallback_EnableForwarding (const struct STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex, bool enable, unsigned int timestamp)
{
	unsigned int i = ENET_MIIReadRegister (20, 6);

	if (portIndex == PORT_RJ45_1)
	{
		i = (i & ~(1ul << 8)) | ((enable != 0) ? (1ul << 8) : 0);
	}
	else if (portIndex == PORT_RJ45_2)
	{
		i = (i & ~(1ul << 9)) | ((enable != 0) ? (1ul << 9) : 0);
	}
	else if (portIndex == PORT_RMII)
	{
		i = (i & ~(1ul << 13)) | ((enable != 0) ? (1ul << 13) : 0);
	}
	else
		assert (0);

	ENET_MIIWriteRegister (20, 6, i);

	update_debug_leds(bridge);
}

static unsigned char BpduFrameBuffer [21 + 36];
static unsigned int BpduFrameSize;

static void* StpCallback_TransmitGetBuffer (const struct STP_BRIDGE* bridge, unsigned int portIndex, unsigned int bpduSize, unsigned int timestamp)
{
	if (portIndex == PORT_RMII)
	{
		// The library is trying to send a BPDU to the RMII port, which we know is us.
		// Let's not allow it cause it would be a waste of bandwidth.
		return NULL;
	}

	assert (21 + bpduSize <= sizeof (BpduFrameBuffer));
	BpduFrameSize = 21 + bpduSize;

	// Dest MAC address
	BpduFrameBuffer[0] = 0x01;
	BpduFrameBuffer[1] = 0x80;
	BpduFrameBuffer[2] = 0xC2;
	BpduFrameBuffer[3] = 0x00;
	BpduFrameBuffer[4] = 0x00;
	BpduFrameBuffer[5] = 0x00;

	// Source Mac Address
	memcpy (&BpduFrameBuffer[6], STP_GetBridgeAddress(bridge)->bytes, 6);
	assert ((unsigned int) BpduFrameBuffer[11] + 1 + portIndex <= 255);
	BpduFrameBuffer[11] += (1 + portIndex);

	// switch chip header
	BpduFrameBuffer[12] = 0x81;
	BpduFrameBuffer[13] = (1 << portIndex);
	BpduFrameBuffer[14] = 0;
	BpduFrameBuffer[15] = 0;

	// EtherType/Size
	BpduFrameBuffer[16] = 0;
	BpduFrameBuffer[17] = 3 + bpduSize;

	// LLC field
	BpduFrameBuffer[18] = 0x42;
	BpduFrameBuffer[19] = 0x42;
	BpduFrameBuffer[20] = 0x03;

	return &BpduFrameBuffer[21];
}

static void StpCallback_TransmitReleaseBuffer (const struct STP_BRIDGE* bridge, void* bufferReturnedByGetBuffer)
{
	tapdev_send (BpduFrameBuffer, BpduFrameSize);
}

static void StpCallback_FlushFdb (const struct STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex, enum STP_FLUSH_FDB_TYPE flushType)
{
	// quickly age out everything
	ENET_MIIWriteRegister (20, 14, 0x60);

	// wait 2 ms while the IC ages out the table
	scheduler_wait(3);

	// reenable slow aging (~5 min)
	ENET_MIIWriteRegister (20, 14, 5);
}

static void StpCallback_DebugStrOut (const struct STP_BRIDGE* bridge, int portIndex, int treeIndex, const char* nullTerminatedString, unsigned int stringLength, unsigned int flush)
{
	printf ("%s", nullTerminatedString);
	if (flush)
		fflush (stdout);
}

// See long comment at the end of 802_1Q_2011_procedures.cpp.
static void StpCallback_OnTopologyChange (const struct STP_BRIDGE* bridge, unsigned int treeIndex, unsigned int timestamp)
{
	// do nothing in this demo app
	//printf ("TC\r\n");
}

// See long comment at the end of 802_1Q_2011_procedures.cpp.
static void StpCallback_OnNotifiedTC (const struct STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex, unsigned int timestamp)
{
	// quickly age out everything
	ENET_MIIWriteRegister (20, 14, 0x60);

	// wait 2 ms while the IC ages out the table
	scheduler_wait(3);

	// reenable slow aging (~5 min)
	ENET_MIIWriteRegister (20, 14, 5);
}

static void StpCallback_OnPortRoleChanged (const struct STP_BRIDGE* bridge, unsigned int portIndex, unsigned int treeIndex, enum STP_PORT_ROLE role, unsigned int timestamp)
{
}

static void* StpCallback_AllocAndZeroMemory (unsigned int size)
{
	void* result = malloc (size);
	assert (result != NULL);
	memset (result, 0, size);
	return result;
}

static void StpCallback_FreeMemory (void* p)
{
	free (p);
}

extern STP_CALLBACKS const callbacks_ip175d =
{
	StpCallback_EnableBpduTrapping,
	StpCallback_EnableLearning,
	StpCallback_EnableForwarding,
	StpCallback_TransmitGetBuffer,
	StpCallback_TransmitReleaseBuffer,
	StpCallback_FlushFdb,
	StpCallback_DebugStrOut,
	StpCallback_OnTopologyChange,
	StpCallback_OnNotifiedTC,
	StpCallback_OnPortRoleChanged,
	StpCallback_AllocAndZeroMemory,
	StpCallback_FreeMemory
};

