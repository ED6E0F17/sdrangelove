// Copyright (C) 2012 maintech GmbH, Otto-Hahn-Str. 15, 97204 Hoechberg, Germany //
// (C) 2015 John Greb                                                            //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////
//
// FM demodulation from rtl_fm Copyright (C) 2012 by Kyle Keen <keenerd@gmail.com>

#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include "tcpsrc.h"
#include "tcpsrcgui.h"

MESSAGE_CLASS_DEFINITION(TCPSrc::MsgTCPSrcConfigure, Message)
MESSAGE_CLASS_DEFINITION(TCPSrc::MsgTCPSrcConnection, Message)
MESSAGE_CLASS_DEFINITION(TCPSrc::MsgTCPSrcSpectrum, Message)

TCPSrc::TCPSrc(MessageQueue* uiMessageQueue, TCPSrcGUI* tcpSrcGUI, SampleSink* spectrum)
{
	m_rig = NULL;
	m_inputSampleRate = 96000;
	m_inputBandwidth = 96000;
	m_sampleFormat = FormatSSB;
	m_outputSampleRate = 48000;
	m_rfBandwidth = 32000;
	m_tcpPort = 9999;
	m_nco.setFreq(0, m_inputSampleRate);
	m_interpolator.create(16, m_inputSampleRate, m_rfBandwidth / 2.0);
	m_sampleDistanceRemain = m_inputSampleRate / m_outputSampleRate;
	m_uiMessageQueue = uiMessageQueue;
	m_tcpSrcGUI = tcpSrcGUI;
	m_spectrum = spectrum;
	m_spectrumEnabled = false;
	m_nextSSBId = 0;
	m_nextS16leId = 0;

	m_last = 0;
	m_this = 0;
	m_dc = 0;
	m_sampleBufferSSB.resize(tcpFftLen);
	TCPFilter = new fftfilt(0.3 / 48.0, 16.0 / 48.0, tcpFftLen);
	// if (!TCPFilter) segfault;
}

TCPSrc::~TCPSrc()
{
	if (TCPFilter) delete TCPFilter;
}

void TCPSrc::configure(MessageQueue* messageQueue, SampleFormat sampleFormat, Real outputSampleRate, Real rfBandwidth, int tcpPort, int boost)
{
	Message* cmd = MsgTCPSrcConfigure::create(sampleFormat, outputSampleRate, rfBandwidth, tcpPort, boost);
	cmd->submit(messageQueue, this);
}

void TCPSrc::setSpectrum(MessageQueue* messageQueue, bool enabled)
{
	Message* cmd = MsgTCPSrcSpectrum::create(enabled);
	cmd->submit(messageQueue, this);
}

int TCPSrc::polar_discriminant(Complex a, Complex b)
{
	Real cr, cj, angle;
	cr = a.real() * b.real() + a.imag() * b.imag();
	cj = a.imag() * b.real() - a.real() * b.imag();
	angle = atan2(cj, cr);
	return (int)(angle * (1<<14) / M_PI);
}

void TCPSrc::feed(SampleVector::const_iterator begin, SampleVector::const_iterator end, bool positiveOnly)
{
	Complex ci;
	cmplx* sideband;
	Real l, r;

	m_sampleBuffer.clear();

	// Rtl-Sdr uses full 16-bit scale; FCDPP does not
	int rescale = 30000 * (1 << m_boost);

	for(SampleVector::const_iterator it = begin; it < end; ++it) {
		Complex c(it->real() / 32768.0, it->imag() / 32768.0);
		c *= m_nco.nextIQ();

		if(m_interpolator.interpolate(&m_sampleDistanceRemain, c, &ci)) {
			m_sampleBuffer.push_back(Sample(ci.real() * rescale, ci.imag() * rescale));
			m_sampleDistanceRemain += m_inputSampleRate / m_outputSampleRate;
		}
	}

	if((m_spectrum != NULL) && (m_spectrumEnabled))
		m_spectrum->feed(m_sampleBuffer.begin(), m_sampleBuffer.end(), positiveOnly);

	for(int i = 0; i < m_s16leSockets.count(); i++)
		m_s16leSockets[i].socket->write((const char*)&m_sampleBuffer[0], m_sampleBuffer.size() * 4);

	if((m_sampleFormat == FormatSSB) && (m_ssbSockets.count() > 0)) {
		for(SampleVector::const_iterator it = m_sampleBuffer.begin(); it != m_sampleBuffer.end(); ++it) {
			Complex cj(it->real() / 30000.0, it->imag() / 30000.0);
			int n_out = TCPFilter->runSSB(cj, &sideband, true);
			if (n_out) {
				for (int i = 0; i < n_out; i+=2) {
					l = (sideband[i].real() + sideband[i].imag()) * 0.7 * 32000.0;
					r = (sideband[i+1].real() + sideband[i+1].imag()) * 0.7 * 32000.0;
					m_sampleBufferSSB.push_back(Sample(l, r));
				}
				for(int i = 0; i < m_ssbSockets.count(); i++)
					m_ssbSockets[i].socket->write((const char*)&m_sampleBufferSSB[0], n_out * 2);
				m_sampleBufferSSB.clear();
			}
		}
	}

	if((m_sampleFormat == FormatNFM) && (m_ssbSockets.count() > 0)) {
		for(SampleVector::const_iterator it = m_sampleBuffer.begin(); it != m_sampleBuffer.end(); ++it) {
			Complex cj(it->real() / 30000.0, it->imag() / 30000.0);
			// An FFT filter here is overkill, but was already set up for SSB
			int n_out = TCPFilter->runFilt(cj, &sideband);
			if (n_out) {
				Real sum = 0.0;
				for (int i = 0; i < n_out; i+=2) {
					m_this = sideband[i];
					l = polar_discriminant(m_this, m_last);
					m_last = sideband[i+1];
					r = polar_discriminant(m_last,  m_this);
					sum += l + r;
					m_sampleBufferSSB.push_back(Sample(l - m_dc, r - m_dc));
				}
				m_dc = int( 0.9 * m_dc + 0.1 * sum / n_out);
				for(int i = 0; i < m_ssbSockets.count(); i++)
					m_ssbSockets[i].socket->write((const char*)&m_sampleBufferSSB[0], n_out * 2);
				m_sampleBufferSSB.clear();
			}
		}
	}
}

void TCPSrc::start()
{
	m_tcpServer = new QTcpServer();
	connect(m_tcpServer, SIGNAL(newConnection()), this, SLOT(onNewConnection()));
	m_tcpServer->listen(QHostAddress::Any, m_tcpPort);
}

void TCPSrc::stop()
{
	closeAllSockets(&m_ssbSockets);
	closeAllSockets(&m_s16leSockets);

	if(m_tcpServer->isListening())
		m_tcpServer->close();
	delete m_tcpServer;
}

bool TCPSrc::handleMessage(Message* cmd)
{
	if(DSPSignalNotification::match(cmd)) {
		DSPSignalNotification* signal = (DSPSignalNotification*)cmd;
		qDebug("%d samples/sec, %lld Hz offset", signal->getSampleRate(), signal->getFrequencyOffset());
		m_inputBandwidth = signal->getTunerBandwidth();
		m_inputSampleRate = signal->getSampleRate();
		if (m_rig) {
			m_rig->setTunerSamples(m_inputBandwidth);
			m_rig->setTunerFreq( signal->getTunerFrequency() );
		}
		m_nco.setFreq(-signal->getFrequencyOffset(), m_inputSampleRate);
		m_interpolator.create(16, m_inputSampleRate, m_rfBandwidth / 2.0);
		m_sampleDistanceRemain = m_inputSampleRate / m_outputSampleRate;
		cmd->completed();
		return true;
	} else if(MsgTCPSrcConfigure::match(cmd)) {
		MsgTCPSrcConfigure* cfg = (MsgTCPSrcConfigure*)cmd;
		m_sampleFormat = cfg->getSampleFormat();
		m_outputSampleRate = cfg->getOutputSampleRate();
		m_rfBandwidth = cfg->getRFBandwidth();
		if(cfg->getTCPPort() != m_tcpPort) {
			m_tcpPort = cfg->getTCPPort();
			if(m_tcpServer->isListening())
				m_tcpServer->close();
			m_tcpServer->listen(QHostAddress::Any, m_tcpPort);
		}
		m_boost = cfg->getBoost();
		m_interpolator.create(16, m_inputSampleRate, m_rfBandwidth / 2.0);
		m_sampleDistanceRemain = m_inputSampleRate / m_outputSampleRate;
		if (m_sampleFormat == FormatSSB)
			TCPFilter->create_filter(0.3 / 48.0, m_rfBandwidth / 2.0 / m_outputSampleRate);
		else
			TCPFilter->create_filter(0.0, m_rfBandwidth / 2.0 / m_outputSampleRate);
		cmd->completed();
		return true;
	} else if(MsgTCPSrcSpectrum::match(cmd)) {
		MsgTCPSrcSpectrum* spc = (MsgTCPSrcSpectrum*)cmd;
		m_spectrumEnabled = spc->getEnabled();
		cmd->completed();
		return true;
	} else {
		if(m_spectrum != NULL)
		   return m_spectrum->handleMessage(cmd);
		else return false;
	}
}

void TCPSrc::closeAllSockets(Sockets* sockets)
{
	for(int i=sockets->count()-1; i>=0; i--) {
		MsgTCPSrcConnection* msg = MsgTCPSrcConnection::create(false, sockets->at(i).id, QHostAddress(), 0);
		msg->submit(m_uiMessageQueue, (PluginGUI*)m_tcpSrcGUI);
		sockets->at(i).socket->close();
	}
}

void TCPSrc::onNewConnection()
{
	while(m_tcpServer->hasPendingConnections()) {
		QTcpSocket* connection = m_tcpServer->nextPendingConnection();
		connect(connection, SIGNAL(disconnected()), this, SLOT(onDisconnected()));

		switch(m_sampleFormat) {

			case FormatNFM:
			case FormatSSB: {
				quint32 id = (FormatSSB << 24) | m_nextSSBId;
				MsgTCPSrcConnection* msg = MsgTCPSrcConnection::create(true, id, connection->peerAddress(), connection->peerPort());
				m_nextSSBId = (m_nextSSBId + 1) & 0xffffff;
				m_ssbSockets.push_back(Socket(id, connection));
				msg->submit(m_uiMessageQueue, (PluginGUI*)m_tcpSrcGUI);
				break;
			}

			case FormatS16LE: {
				quint32 id = (FormatS16LE << 24) | m_nextS16leId;
				MsgTCPSrcConnection* msg = MsgTCPSrcConnection::create(true, id, connection->peerAddress(), connection->peerPort());
				m_nextS16leId = (m_nextS16leId + 1) & 0xffffff;
				m_s16leSockets.push_back(Socket(id, connection));
				msg->submit(m_uiMessageQueue, (PluginGUI*)m_tcpSrcGUI);
				break;
			}

			default:
				delete connection;
				break;
		}
	}
}

void TCPSrc::onDisconnected()
{
	quint32 id;
	QTcpSocket* socket = NULL;

	for(int i = 0; i < m_ssbSockets.count(); i++) {
		if(m_ssbSockets[i].socket == sender()) {
			id = m_ssbSockets[i].id;
			socket = m_ssbSockets[i].socket;
			m_ssbSockets.removeAt(i);
			break;
		}
	}
	if(socket == NULL) {
		for(int i = 0; i < m_s16leSockets.count(); i++) {
			if(m_s16leSockets[i].socket == sender()) {
				id = m_s16leSockets[i].id;
				socket = m_s16leSockets[i].socket;
				m_s16leSockets.removeAt(i);
				break;
			}
		}
	}
	if(socket != NULL) {
		MsgTCPSrcConnection* msg = MsgTCPSrcConnection::create(false, id, QHostAddress(), 0);
		msg->submit(m_uiMessageQueue, (PluginGUI*)m_tcpSrcGUI);
		socket->deleteLater();
	}
}
