/*
	Copyright (C) 2019-2021 Doug McLain

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "codec.h"
#include <iostream>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef USE_FLITE
extern "C" {
extern cst_voice * register_cmu_us_slt(const char *);
extern cst_voice * register_cmu_us_kal16(const char *);
extern cst_voice * register_cmu_us_awb(const char *);
}
#endif

Codec::Codec(QString callsign, char module, QString hostname, QString host, int port, bool ipv6, QString vocoder, QString modem, QString audioin, QString audioout, uint8_t attenuation) :
	m_module(module),
	m_hostname(hostname),
	m_tx(false),
	m_ttsid(0),
	m_audioin(audioin),
	m_audioout(audioout),
	m_rxwatchdog(0),
	m_attenuation(attenuation),
#ifdef Q_OS_WIN
	m_rxtimerint(19),
#else
	m_rxtimerint(20),
#endif
	m_txtimerint(19),
	m_vocoder(vocoder),
	m_modemport(modem),
	m_modem(nullptr),
	m_ambedev(nullptr),
	m_hwrx(false),
	m_hwtx(false),
	m_ipv6(ipv6)
{
	m_modeinfo.callsign = callsign;
	m_modeinfo.gwid = 0;
	m_modeinfo.srcid = 0;
	m_modeinfo.dstid = 0;
	m_modeinfo.host = host;
	m_modeinfo.port = port;
	m_modeinfo.count = 0;
	m_modeinfo.frame_number = 0;
	m_modeinfo.frame_total = 0;
	m_modeinfo.streamid = 0;
	m_modeinfo.stream_state = STREAM_IDLE;
	m_modeinfo.sw_vocoder_loaded = false;
	m_modeinfo.hw_vocoder_loaded = false;
#ifdef USE_FLITE
	flite_init();
	voice_slt = register_cmu_us_slt(nullptr);
	voice_kal = register_cmu_us_kal16(nullptr);
	voice_awb = register_cmu_us_awb(nullptr);
#endif
}

Codec::~Codec()
{
}

void Codec::ambe_connect_status(bool s)
{
	if(s){
#if !defined(Q_OS_IOS)
		m_modeinfo.ambedesc = m_ambedev->get_ambe_description();
		m_modeinfo.ambeprodid = m_ambedev->get_ambe_prodid();
		m_modeinfo.ambeverstr = m_ambedev->get_ambe_verstring();
#endif
	}
	else{
		m_modeinfo.ambeprodid = "Connect failed";
		m_modeinfo.ambeverstr = "Connect failed";
	}
	emit update(m_modeinfo);
}

void Codec::mmdvm_connect_status(bool s)
{
	if(s){
		//m_modeinfo.mmdvmdesc = m_modem->get_mmdvm_description();
#if !defined(Q_OS_IOS)
		m_modeinfo.mmdvm = m_modem->get_mmdvm_version();
#endif
	}
	else{
		m_modeinfo.mmdvm = "Connect failed";
	}
	emit update(m_modeinfo);
}

void Codec::in_audio_vol_changed(qreal v)
{
	m_audio->set_input_volume(v / m_attenuation);
}

void Codec::out_audio_vol_changed(qreal v)
{
	m_audio->set_output_volume(v);
}

void Codec::agc_state_changed(int s)
{
	qDebug() << "Codec::agc_state_changed() called s == " << s;
	m_audio->set_agc(s);
}

void Codec::send_connect()
{
	m_modeinfo.status = CONNECTING;

	if(m_modeinfo.host == "MMDVM_DIRECT"){
		mmdvm_direct_connect();
	}
	else if(m_ipv6 && (m_modeinfo.host != "none")){
		qDebug() << "Host == " << m_modeinfo.host;
		QList<QHostAddress> h;
		QHostInfo i;
		h.append(QHostAddress(m_modeinfo.host));
		i.setAddresses(h);
		hostname_lookup(i);
	}
	else{
		QHostInfo::lookupHost(m_modeinfo.host, this, SLOT(hostname_lookup(QHostInfo)));
	}
}

void Codec::toggle_tx(bool tx)
{
	tx ? start_tx() : stop_tx();
}

void Codec::start_tx()
{
#if !defined(Q_OS_IOS)
	if(m_hwtx){
		m_ambedev->clear_queue();
	}
#endif
	m_txcodecq.clear();
	m_tx = true;
	m_txcnt = 0;
	m_ttscnt = 0;
	m_rxtimer->stop();
	m_modeinfo.streamid = 0;
	m_modeinfo.stream_state = TRANSMITTING;
#ifdef USE_FLITE

	if(m_ttsid == 1){
		tts_audio = flite_text_to_wave(m_ttstext.toStdString().c_str(), voice_kal);
	}
	else if(m_ttsid == 2){
		tts_audio = flite_text_to_wave(m_ttstext.toStdString().c_str(), voice_awb);
	}
	else if(m_ttsid == 3){
		tts_audio = flite_text_to_wave(m_ttstext.toStdString().c_str(), voice_slt);
	}
#endif
	if(!m_txtimer->isActive()){
		if(m_ttsid == 0){
			m_audio->set_input_buffer_size(640);
			m_audio->start_capture();
			//audioin->start(&audio_buffer);
		}
		m_txtimer->start(m_txtimerint);
	}
}

void Codec::stop_tx()
{
	m_tx = false;
}

bool Codec::load_vocoder_plugin()
{
#ifdef VOCODER_PLUGIN
	QString config_path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_WIN)
	config_path += "/dudetronics";
#endif
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
	QString voc = config_path + "/vocoder_plugin." + QSysInfo::productType() + "." + QSysInfo::currentCpuArchitecture();
#else
	QString voc = config_path + "/vocoder_plugin." + QSysInfo::kernelType() + "." + QSysInfo::currentCpuArchitecture();
#endif
#if !defined(Q_OS_WIN)
	//QString voc = "/mnt/data/src/mbe_vocoder/vocoder_plugin.linux.x86_64.so";
	void* a = dlopen(voc.toLocal8Bit(), RTLD_LAZY);
	if (!a) {
		qDebug() << "Cannot load library: " << QString::fromLocal8Bit(dlerror());
		return false;
	}
	dlerror();

	create_t* create_a = (create_t*) dlsym(a, "create");
	const char* dlsym_error = dlerror();

	if (dlsym_error) {
		qDebug() << "Cannot load symbol create: " << QString::fromLocal8Bit(dlsym_error);
		return false;
	}

	m_mbevocoder = create_a();
	qDebug() << voc + " loaded";
	return true;
#else
	HINSTANCE hinstvoclib;
	hinstvoclib = LoadLibrary(reinterpret_cast<LPCWSTR>(voc.utf16()));

	if (hinstvoclib != NULL) {
		create_t* create_a = (create_t*)GetProcAddress(hinstvoclib, "create");

		if (create_a != NULL) {
			m_mbevocoder = create_a();
			qDebug() << voc + " loaded";
			return true;
		}
		else{
			return false;
		}
	}
	else{
		return false;
	}
#endif
#else
#if defined(Q_OS_IOS)
	if(QFileInfo::exists(voc)){
		m_mbevocoder = new VocoderPlugin();
		return true;
	}
	else{
		return false;
	}
#else
	m_mbevocoder = new VocoderPlugin();
	return true;
#endif
#endif
}

void Codec::deleteLater()
{
	if(m_modeinfo.status == CONNECTED_RW){
		//m_udp->disconnect();
		//m_ping_timer->stop();
		send_disconnect();
		delete m_audio;
#if !defined(Q_OS_IOS)
		if(m_hwtx){
			delete m_ambedev;
		}
		if(m_modem){
			delete m_modem;
		}
#endif
	}
	m_modeinfo.count = 0;
	QObject::deleteLater();
}
