///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2012 maintech GmbH, Otto-Hahn-Str. 15, 97204 Hoechberg, Germany //
// written by Christian Daniel                                                   //
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

#include <QTime>
#include <stdio.h>
#include "nfmdemod.h"
#include "dsp/dspcommands.h"

MESSAGE_CLASS_DEFINITION(NFMDemod::MsgConfigureNFMDemod, Message)

NFMDemod::NFMDemod(SampleSink* sampleSink) :
	m_sampleSink(sampleSink)
{
	m_rfBandwidth = 36000;
	m_volume = 2.0;
	m_squelchLevel = pow(10.0, -40.0 / 10.0);
	m_sampleRate = 48000;
	m_frequency = 0;
	m_scale = 0;
	m_framedrop = 0;

	m_nco.setFreq(m_frequency, m_sampleRate);
	m_interpolator.create(16, m_sampleRate, 18000);
	m_sampleDistanceRemain = (Real)m_sampleRate / 48000.0;

	m_movingAverage.resize(16, 0);
}

NFMDemod::~NFMDemod()
{
}

void NFMDemod::configure(MessageQueue* messageQueue, Real rfBandwidth, Real afBandwidth, Real volume, Real squelch)
{
	Message* cmd = MsgConfigureNFMDemod::create(rfBandwidth, afBandwidth, volume, squelch);
	cmd->submit(messageQueue, this);
}

void NFMDemod::feed(SampleVector::const_iterator begin, SampleVector::const_iterator end, bool positiveOnly)
{
	Complex ci;
	qint16 sample;
	Real a, b, s, demod;
	double meansqr = 1.0;

	for(SampleVector::const_iterator it = begin; it < end; ++it) {
		Complex c(it->real() / 32768.0, it->imag() / 32768.0);
		c *= m_nco.nextIQ();

		if(m_interpolator.interpolate(&m_sampleDistanceRemain, c, &ci)) {
			s = ci.real() * ci.real() + ci.imag() * ci.imag();
			meansqr += s;
			m_movingAverage.feed(s);
			if(m_movingAverage.average() >= m_squelchLevel)
				m_squelchState = m_sampleRate / 50;

			a = m_scale * m_this.real() * (m_last.imag() - ci.imag());
			b = m_scale * m_this.imag() * (m_last.real() - ci.real());
			m_last = m_this;
			m_this = Complex(ci.real(), ci.imag());

			demod = m_volume * (b - a);
			sample = demod * 30000;

			// Display audio spectrum to 50%
			if (++m_framedrop & 1)
				m_sampleBuffer.push_back(Sample(sample, sample));

			if(m_squelchState > 0)
				m_squelchState--;

			m_sampleDistanceRemain += (Real)m_sampleRate / 48000.0;
		}
	}

	if(m_sampleSink != NULL)
		m_sampleSink->feed(m_sampleBuffer.begin(), m_sampleBuffer.end(), true);

	// TODO: correct levels
	m_scale = ( end - begin) * m_sampleRate / 48000 / meansqr;
}

void NFMDemod::start()
{
	m_squelchState = 0;
}

void NFMDemod::stop()
{
}

bool NFMDemod::handleMessage(Message* cmd)
{
	if(DSPSignalNotification::match(cmd)) {
		DSPSignalNotification* signal = (DSPSignalNotification*)cmd;
		qDebug("%d samples/sec, %lld Hz offset", signal->getSampleRate(), signal->getFrequencyOffset());
		m_sampleRate = signal->getSampleRate();
		m_nco.setFreq(-signal->getFrequencyOffset(), m_sampleRate);
		m_interpolator.create(16, m_sampleRate, m_rfBandwidth / 2.1);
		m_sampleDistanceRemain = m_sampleRate / 48000.0;
		m_squelchState = 0;
		cmd->completed();
		return true;
	} else if(MsgConfigureNFMDemod::match(cmd)) {
		MsgConfigureNFMDemod* cfg = (MsgConfigureNFMDemod*)cmd;
		m_rfBandwidth = cfg->getRFBandwidth();
		m_interpolator.create(16, m_sampleRate, m_rfBandwidth / 2.1);
		m_squelchLevel = pow(10.0, cfg->getSquelch() / 10.0);
		m_volume = cfg->getVolume();
		cmd->completed();
		return true;
	} else {
		if(m_sampleSink != NULL)
		   return m_sampleSink->handleMessage(cmd);
		else return false;
	}
}
