// license:BSD-3-Clause
// copyright-holders:Eric Anderson
/***************************************************************************

Vector Graphic had two related disk controllers for the Vector 4. There was
the "dual-mode" ST506-interface HDD/5.25" FDD controller and a stripped-down
5.25" FDD-only controller. Both can handle four FDD. The dual-mode version
supports a HDD as drive 0, replacing a FDD when used.

The floppy and hard drive formatting is not IBM compatible. Instead they are
based on the Micropolis MFM hard-sectored format which starts and ends the
sector with 0x00 preamble and postable bytes and starts sector data with a
0xFF sync byte. The FDD has 16 hard sectors, but the HDD uses a normal
soft-sectored drive with a PLL on the controller to emulate 32 hard sectors.
No abnormal MFM clock bits are used.

https://www.bitsavers.org/pdf/vectorGraphic/hardware/7200-1200-02-1_Dual-Mode_Disk_Controller_Board_Engineering_Documentation_Feb81.pdf
https://archive.org/details/7200-0001-vector-4-technical-information-sep-82

TODO:
- ECC

****************************************************************************/

#include "emu.h"
#include "vectordualmode.h"

#include "formats/micropolis_hd.h"
#include "formats/vgi_dsk.h"

static const attotime fdd_half_bitcell_size = attotime::from_usec(2);
static const attotime hdd_half_bitcell_size = attotime::from_nsec(100);

/* Interleave 8 bits with zeros. abcdefgh -> 0a0b0c0d0e0f0g0h */
static int deposit8(int data)
{
	int d = data;
	d = ((d & 0xf0) << 4) | (d & 0x0f);
	d = ((d << 2) | d) & 0x3333;
	d = ((d << 1) | d) & 0x5555;
	return d;
}

static uint16_t mfm_byte(uint8_t data, unsigned int prev_data)
{
	const unsigned int ext_data = data | (prev_data << 8);
	const unsigned int clock = ~(ext_data | (ext_data >> 1));
	return (deposit8(clock) << 1) | deposit8(ext_data);
}

static uint8_t unmfm_byte(uint16_t mfm)
{
	unsigned int d = mfm;
	d &= 0x5555;
	d = ((d >> 1) | d) & 0x3333;
	d = ((d >> 2) | d) & 0x0f0f;
	d = ((d >> 4) | d) & 0x00ff;
	return d;
}

s100_vector_dualmode_device::s100_vector_dualmode_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, S100_VECTOR_DUALMODE, tag, owner, clock)
	, device_s100_card_interface(mconfig, *this)
	, m_floppy(*this, "floppy%u", 0U)
	, m_hdd(*this, "hdd")
	, m_ram{0}
	, m_cmar(0)
	, m_drive(0)
	, m_reduced_wc(false)
	, m_sector(0)
	, m_sector_counter(0)
	, m_read(false)
	, m_ecc(false)
	, m_wpcom(false)
	, m_busy(false)
	, m_last_sector_pulse(attotime::zero)
	, m_last_index_pulse(attotime::zero)
	, m_pll()
	, m_byte_timer(nullptr)
	, m_sector_timer(nullptr)
	, m_pending_byte(0)
	, m_pending_size(0)
{
}

TIMER_CALLBACK_MEMBER(s100_vector_dualmode_device::motor_off)
{
	for (int i = 0; i < m_floppy.size(); i++) {
		floppy_image_device *flop = m_floppy[m_drive]->get_device();
		if (flop)
			flop->mon_w(1);
	}
	m_byte_timer->enable(false);
	m_busy = false;
}

bool s100_vector_dualmode_device::hdd_selected()
{
	return m_drive == 0 && m_hdd->get_device();
}

uint8_t s100_vector_dualmode_device::s100_sinp_r(offs_t offset)
{
	if (m_busy)
		return 0xff;
	// 7200-1200-02-1 page 16 (1-10)
	uint8_t data;
	if (offset == 0xc0) { // status (0) port
		bool write_protect; // FDD
		bool ready; // HDD
		bool track0;
		bool write_fault = false; // HDD
		bool seek_complete; // HDD
		bool loss_of_sync; // HDD
		if (hdd_selected()) {
			mfm_harddisk_device *hdd = m_hdd->get_device();
			write_protect = false;
			ready = hdd->ready_r() == ASSERT_LINE;
			track0 = hdd->trk00_r() == ASSERT_LINE;
			seek_complete = hdd->seek_complete_r() == ASSERT_LINE;
			loss_of_sync = true;
		} else {
			floppy_image_device *flop = m_floppy[m_drive]->get_device();
			write_protect = flop && flop->wpt_r();
			ready = false;
			track0 = flop && !flop->trk00_r();
			seek_complete = false;
			loss_of_sync = false;
		}

		data = (write_protect ? 0x01 : 0)
		    | (ready ? 0x02 : 0)
		    | (track0 ? 0x04 : 0)
		    | (write_fault ? 0x08 : 0)
		    | (seek_complete ? 0x10 : 0)
		    | (loss_of_sync ? 0x20 : 0)
		    | 0xc0;
	} else if (offset == 0xc1) { // status (1) port
		bool floppy_disk_selected;
		bool controller_busy = m_busy; // returned early if true
		bool motor_on; // FDD
		bool type_of_hard_disk = true;
		if (hdd_selected()) {
			floppy_disk_selected = false;
			motor_on = false;
		} else {
			floppy_disk_selected = true;
			motor_on = m_motor_on_timer->enabled();
		}
		data = (floppy_disk_selected ? 0x01 : 0)
		    | (controller_busy ? 0x02 : 0)
		    | (motor_on ? 0x04 : 0)
		    | (type_of_hard_disk ? 0x08 : 0)
		    | 0xf0;
	} else if (offset == 0xc2) { // data port
		data = m_ram[m_cmar];
		if (!machine().side_effects_disabled()) {
			m_cmar++;
			m_cmar &= 0x1ff;
		}
	} else if (offset == 0xc3) { // reset port
		if (!machine().side_effects_disabled())
			m_cmar = 0;
		data = 0xff;
	} else {
		data = 0xff;
	}
	return data;
}

void s100_vector_dualmode_device::s100_sout_w(offs_t offset, uint8_t data)
{
	if (m_busy)
		return;
	// 7200-1200-02-1 page 14 (1-8)
	if (offset == 0xc0) { // control (0) port
		m_drive = BIT(data, 0, 2);
		const uint8_t head = BIT(data, 2, 3);
		const bool step = BIT(data, 5);
		const bool step_in = BIT(data, 6);
		m_reduced_wc = BIT(data, 7); // HDD

		for (int i = 0; i < m_floppy.size(); i++) {
			floppy_image_device *flop = m_floppy[m_drive]->get_device();
			if (flop)
				flop->mon_w(0);
		}
		// WR0| triggers U60, a 74LS123 with 100uF cap and 100k res
		m_motor_on_timer->adjust(attotime::from_usec(2819600));

		if (hdd_selected()) {
			mfm_harddisk_device *hdd = m_hdd->get_device();
			hdd->headsel_w(head & (hdd->get_actual_heads()-1));
			hdd->step_w(step ? ASSERT_LINE : CLEAR_LINE);
			hdd->direction_in_w(step_in ? ASSERT_LINE : CLEAR_LINE);
			hdd->setup_index_pulse_cb(mfm_harddisk_device::index_pulse_cb(&s100_vector_dualmode_device::harddisk_index_cb, this));
		} else {
			floppy_image_device *flop = m_floppy[m_drive]->get_device();
			if (flop) {
				flop->ss_w(head & 1);
				// Software should not change other bits when pulsing step
				flop->stp_w(!step);
				flop->dir_w(!step_in);
				flop->setup_index_pulse_cb(floppy_image_device::index_pulse_cb(&s100_vector_dualmode_device::floppy_index_cb, this));
			}
			if (m_sector_timer->enabled())
				m_sector_timer->reset();
		}
	} else if (offset == 0xc1) { // control (1) port
		m_sector = BIT(data, 0, 5);
		m_read = BIT(data, 5);
		m_ecc = BIT(data, 6);
		m_wpcom = BIT(data, 7); // HDD
	} else if (offset == 0xc2) { // data port
		m_ram[m_cmar++] = data;
		m_cmar &= 0x1ff;
	} else if (offset == 0xc3) { // start port
		m_busy = m_motor_on_timer->enabled();
	}
}

bool s100_vector_dualmode_device::get_next_bit(attotime &tm, const attotime &limit)
{
	int bit = m_pll.get_next_bit(tm, m_floppy[m_drive]->get_device(), limit);
	if (bit < 0)
		return false;
	m_pending_byte <<= 1;
	m_pending_byte |= bit;
	m_pending_size++;
	return true;
}

void s100_vector_dualmode_device::floppy_index_cb(floppy_image_device *floppy, int state)
{
	if (hdd_selected() || m_floppy[m_drive]->get_device() != floppy)
		return;
	if (!state)
		return;
	attotime now = machine().time();
	// U25 74LS221: 61.9 KOhm * .22 uF * .75
	if (now - m_last_sector_pulse < attotime::from_nsec(10213500)) {
		m_sector_counter = 0xf;
	} else {
		m_last_sector_pulse = now;
		sector_cb(0);
	}
}

void s100_vector_dualmode_device::harddisk_index_cb(mfm_harddisk_device *hdd, int state)
{
	if (!hdd_selected())
		return;
	if (!state)
		return;
	// U20 4046 PLL for 32x multiplier
	attotime now = machine().time();
	m_sector_counter = 0x1f;
	attotime sector_time = (now - m_last_index_pulse)/32;
	m_last_index_pulse = now;
	m_sector_timer->adjust(sector_time, 0, sector_time);
	sector_cb(0);
}

TIMER_CALLBACK_MEMBER(s100_vector_dualmode_device::sector_cb)
{
	m_sector_counter++;
	m_sector_counter &= hdd_selected() ? 0x1f : 0xf;
	if (hdd_selected() && m_sector_counter == 0x1f)
		m_sector_timer->reset(); // wait for IDX
	if (!m_busy)
		return;

	if (m_byte_timer->enabled()) {
		// op completed
		m_byte_timer->enable(false);
		m_busy = false;
		if (m_read && !hdd_selected())
			m_ram[274] = 0; // Ignore ECC
		return;
	}

	if (m_sector_counter == m_sector) {
		if (m_read) {
			if (hdd_selected()) {
				attotime tm = machine().time() + hdd_half_bitcell_size*256;
				attotime limit = tm + hdd_half_bitcell_size*16*30;
				while (m_pending_byte != 0x5555) {
					if (m_hdd->get_device()->read(tm, limit, m_pending_byte))
						return;
				}
				m_pending_size = 16;
				m_byte_timer->adjust(tm - machine().time());
			} else {
				m_pll.set_clock(fdd_half_bitcell_size);
				m_pll.read_reset(machine().time());
				attotime tm;
				attotime limit = machine().time() + fdd_half_bitcell_size*512;
				while (get_next_bit(tm, limit)) {} // init PLL
				limit += fdd_half_bitcell_size*16*30;
				while (m_pending_byte != 0x5554) {
					if (!get_next_bit(tm, limit))
						return;
				}
				m_pending_size = 1;
				m_byte_timer->adjust(tm - machine().time());
			}
		} else {
			m_pending_size = 0;
			m_byte_timer->adjust(attotime::zero);
		}
	}
}

TIMER_CALLBACK_MEMBER(s100_vector_dualmode_device::byte_cb)
{
	if (m_read) {
		if (m_pending_size == 16) {
			m_pending_size = 0;
			m_ram[m_cmar++] = unmfm_byte(m_pending_byte);
			m_cmar &= 0x1ff;
		}
		attotime tm = machine().time();
		if (hdd_selected()) {
			m_hdd->get_device()->read(tm, attotime::never, m_pending_byte);
			m_pending_size = 16;
		} else {
			while (m_pending_size != 16 && get_next_bit(tm, attotime::never)) {}
		}
		m_byte_timer->adjust(tm - machine().time());
	} else {
		const attotime half_bitcell_size = hdd_selected() ? hdd_half_bitcell_size : fdd_half_bitcell_size;
		if (m_pending_size == 16) {
			attotime start_time = machine().time() - half_bitcell_size*m_pending_size;
			attotime tm = start_time;
			if (hdd_selected()) {
				m_hdd->get_device()->write(tm, attotime::never, m_pending_byte, m_wpcom, m_reduced_wc);
			} else {
				attotime buf[8];
				int pos = 0;
				while (m_pending_size) {
					if (m_pending_byte & (1 << --m_pending_size))
						buf[pos++] = tm + half_bitcell_size/2;
					tm += half_bitcell_size;
				}
				floppy_image_device *floppy = m_floppy[m_drive]->get_device();
				if (floppy)
					floppy->write_flux(start_time, machine().time(), pos, buf);
			}
		}
		uint8_t last = m_cmar ? m_ram[m_cmar-1] : 0;
		m_pending_byte = mfm_byte(m_ram[m_cmar++], last);
		m_pending_size = 16;
		m_cmar &= 0x1ff;
		m_byte_timer->adjust(half_bitcell_size*16);
	}
}

void s100_vector_dualmode_device::device_start()
{
	m_motor_on_timer = timer_alloc(FUNC(s100_vector_dualmode_device::motor_off), this);
	m_byte_timer = timer_alloc(FUNC(s100_vector_dualmode_device::byte_cb), this);
	m_sector_timer = timer_alloc(FUNC(s100_vector_dualmode_device::sector_cb), this);

	if (m_floppy[0]->get_device())
		m_floppy[0]->get_device()->setup_index_pulse_cb(floppy_image_device::index_pulse_cb(&s100_vector_dualmode_device::floppy_index_cb, this));
	if (m_floppy[1]->get_device())
		m_floppy[1]->get_device()->setup_index_pulse_cb(floppy_image_device::index_pulse_cb(&s100_vector_dualmode_device::floppy_index_cb, this));
	if (m_floppy[2]->get_device())
		m_floppy[2]->get_device()->setup_index_pulse_cb(floppy_image_device::index_pulse_cb(&s100_vector_dualmode_device::floppy_index_cb, this));
	if (m_floppy[3]->get_device())
		m_floppy[3]->get_device()->setup_index_pulse_cb(floppy_image_device::index_pulse_cb(&s100_vector_dualmode_device::floppy_index_cb, this));
	if (m_hdd->get_device())
		m_hdd->get_device()->setup_index_pulse_cb(mfm_harddisk_device::index_pulse_cb(&s100_vector_dualmode_device::harddisk_index_cb, this));

	save_item(NAME(m_ram));
	save_item(NAME(m_cmar));
	save_item(NAME(m_drive));
	save_item(NAME(m_reduced_wc));
	save_item(NAME(m_sector));
	save_item(NAME(m_sector_counter));
	save_item(NAME(m_read));
	save_item(NAME(m_ecc));
	save_item(NAME(m_wpcom));
	save_item(NAME(m_busy));
	save_item(NAME(m_pending_byte));
	save_item(NAME(m_pending_size));
}

void s100_vector_dualmode_device::device_reset()
{
	// POC| resets
	// U9
	m_drive = 0;
	// U18
	m_sector = 0;
	m_read = false;
	// U60
	m_motor_on_timer->enable(false);
}

static void vector4_floppies(device_slot_interface &device)
{
	device.option_add("525", FLOPPY_525_QD);
}

static void vector4_formats(format_registration &fr)
{
	fr.add_mfm_containers();
	fr.add(FLOPPY_VGI_FORMAT);
}

static void vector4_harddisks(device_slot_interface &device)
{
	device.option_add("generic", MFMHD_GENERIC);
	device.option_add("st406", MFMHD_ST406);     // 5 MB; single platter
	device.option_add("st412", MFMHD_ST412);     // 10 MB
	device.option_add("st506", MFMHD_ST506);     // 5 MB; double platter
}

void s100_vector_dualmode_device::device_add_mconfig(machine_config &config)
{
	FLOPPY_CONNECTOR(config, m_floppy[0], vector4_floppies, nullptr, vector4_formats).enable_sound(true);
	FLOPPY_CONNECTOR(config, m_floppy[1], vector4_floppies, "525", vector4_formats).enable_sound(true);
	FLOPPY_CONNECTOR(config, m_floppy[2], vector4_floppies, nullptr, vector4_formats).enable_sound(true);
	FLOPPY_CONNECTOR(config, m_floppy[3], vector4_floppies, nullptr, vector4_formats).enable_sound(true);

	MFM_HD_CONNECTOR(config, m_hdd, vector4_harddisks, "generic", MFM_BYTE, 3000, 20, MFMHD_MICROPOLIS_FORMAT);
}

DEFINE_DEVICE_TYPE(S100_VECTOR_DUALMODE, s100_vector_dualmode_device, "vectordualmode", "Vector Dual-Mode Disk Controller")
