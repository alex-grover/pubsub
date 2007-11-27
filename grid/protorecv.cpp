// protorecv.cpp

#include "stdafx.h"
#include "protocol.h"
#include "boost/thread.hpp"
#include "../util/goodies.h"
#include "../util/sock.h"
#include "protoimpl.h"

boost::mutex coutmutex;
boost::mutex mutexr;
boost::condition threadActivate; // creating a new thread, grabbing threadUseThisOne
ProtocolConnection *threadUseThisOne = 0;
void receiverThread();

map<SockAddr,ProtocolConnection*> firstPCForThisAddress;

EndPointToPC pcMap;

void ProtocolConnection::shutdown() { 
	ptrace( cout << ".shutdown()" << endl; )
	if( acceptAnyChannel() || first ) 
		return;
	ptrace( cout << ".   to:" << to.toString() << endl; )
	__sendRESET(this, to);
}

inline bool ProtocolConnection::acceptAnyChannel() const { 
	return myEnd.channel == MessagingPort::ANYCHANNEL; 
}

void ProtocolConnection::init() {
	first = true;
	lock lk(mutexr);
	if( firstPCForThisAddress.count(myEnd.sa) == 0 ) { 
		firstPCForThisAddress[myEnd.sa] = this;
		// need a receiver thread
		boost::thread receiver(receiverThread);
		threadUseThisOne = this;
		pcMap[myEnd] = this;
		threadActivate.notify_one();
		return;
	}
	pcMap[myEnd] = this;
}

/* find message for fragment */
MR* CR::getPendingMsg(F *fr, EndPoint& fromAddr) {
	MR *m;
	map<int,MR*>::iterator i = pendingMessages.find(fr->__msgid());
	if( i == pendingMessages.end() ) {
		if( pendingMessages.size() > 20 ) {		
			cout << ".warning: pendingMessages.size()>20, ignoring msg until we dequeue" << endl;
			return 0;
		}
		m = new MR(&pc, fr->__msgid(), fromAddr);
		pendingMessages[fr->__msgid()] = m;
	}
	else
		m = i->second;
	return m;
/*
	MR*& m = pendingMessages[fr->__msgid()];
	if( m == 0 )		
		m = new MR(&pc, fr->__msgid(), fromAddr);
	return m;
*/
}

void MR::removeFromReceivingList() {
	pc.cr.pendingMessages.erase(msgid);
}

MR::MR(ProtocolConnection *_pc, MSGID _msgid, EndPoint& _from) : 
        pc(*_pc), msgid(_msgid), from(_from), nReceived(0)
{
	messageLenExpected = nExpected = -1;
	f.push_back(0);
}

void MR::reportMissings(bool reportAll) { 
	vector<short> missing;
	unsigned t = curTimeMillis();
	for( unsigned i = 0; i < f.size(); i++ ) { 
		if( f[i] == 0 ) {
			int diff = tdiff(reportTimes[i],t);
			assert( diff >= 0 || reportTimes[i] == 0 );
			if( diff > 100 || reportTimes[i]==0 || 
				(reportAll && diff > 1) ) {
				reportTimes[i] = t;
				assert( i < 25000 );
				missing.push_back(i);
			}
		}
	}
	if( !missing.empty() )
		__sendMISSING(&pc, from, msgid, missing);
}

inline bool MR::complete() { 
	if( nReceived == nExpected ) {
		assert( nExpected == (int) f.size() );
		assert( f[0] != 0 );
		return true;
	}
	return false;
/*
	if( nExpected > (int) f.size() || nExpected == -1 )
		return false;
	for( unsigned i = 0; i < f.size(); i++ )
		if( f[i] == 0 )
			return false;
	return true;
*/
}

/* true=msg complete */
bool MR::got(F *frag, EndPoint& fromAddr) {
	MSGID msgid = frag->__msgid();
	int i = frag->__num();
	if( i == 544 && !frag->__isREQUESTACK() ) { 
		cout << "************ GOT LAST FRAGMENT #544" << endl;
	}
	if( nExpected < 0 && i == 0 ) {
		messageLenExpected = frag->__firstFragMsgLen();
		if( messageLenExpected == frag->__len()-FragHeader )
			nExpected = 1;
		else {
			int mss = frag->__len()-FragHeader;
			assert( messageLenExpected > mss );
			nExpected = (messageLenExpected + mss - 1) / mss;
			ptrace( cout << ".got first frag, expect:" << nExpected << "packets, expectedLen:" << messageLenExpected << endl; )
		}
	}
	if( i >= (int) f.size() )
		f.resize(i+1, 0);
	if( frag->__isREQUESTACK() ) { 
		ptrace( cout << "...got(): processing a REQUESTACK" << endl; )
		/* we're simply seeing if we got the data, and then acking. */
		delete frag;
		frag = 0;
		reportTimes.resize(f.size(), 0);
		if( complete() ) {
			__sendACK(&pc, fromAddr, msgid);
			return true;
		} 
		reportMissings(true);
		return false;
	}
	else if( f[i] != 0 ) { 
		ptrace( cout << "\t.dup packet i:" << i << ' ' << from.toString() << endl; )
		delete frag;
		return false;
	}
	if( frag ) { 
		f[i] = frag;
		nReceived++;
	}
	reportTimes.resize(f.size(), 0);

	if( !complete() )
		return false;
	__sendACK(&pc, fromAddr, msgid);
	return true;

/*
	if( f[0] == 0 || f[i] == 0 ) {
//		reportMissings(frag == 0);
		return false;
	}
	if( i+1 < nExpected ) {
//cout << "TEMP COMMENT" << endl;
//		if( i > 0 && f[i-1] == 0 )
//			reportMissings(frag == 0);
		return false;
	}
	// last fragment received
	if( !complete() ) {
//		reportMissings(frag == 0);
		return false;
	}
	__sendACK(&pc, fromAddr, msgid);
	return frag != 0;
*/
}

MR* CR::recv() { 
	MR *x;
	{
		lock lk(mutexr);
		while( received.empty() )
			receivedSome.wait(lk);
		x = received.back();
		received.pop_back();
	}
	ptrace( cout << "TEMP: CR:recv complete! ***********************************"; )
	return x;
}

// this is how we tell protosend.cpp we got these
void gotACK(F*, ProtocolConnection *, EndPoint&);
void gotMISSING(F*, ProtocolConnection *, EndPoint&);

void receiverThread() {
	ProtocolConnection *mypc;
	{
		lock lk(mutexr);
		while( 1 ) {
			if( threadUseThisOne != 0 ) {
				mypc = threadUseThisOne;
				threadUseThisOne = 0;
				break;
			}
			threadActivate.wait(lk);
		}
	}

	EndPoint fromAddr;
	while( 1 ) {
		F *f = __recv(mypc->udpConnection, fromAddr.sa);
		fromAddr.channel = f->__channel();
		ptrace( cout << "..__recv() from:" << fromAddr.toString() << " frag:" << f->__num() << endl; )
		ProtocolConnection *pc = mypc;
		if( fromAddr.channel != pc->myEnd.channel ) {
			if( !pc->acceptAnyChannel() ) { 
				ptrace( cout << ".WARNING: wrong channel\n"; )
				ptrace( cout << ".  expected:" << pc->myEnd.channel << " got:" << fromAddr.channel << '\n'; )
				ptrace( cout << ".  this may be ok if you just restarted" << endl; )
				delete f;
				continue;
			}
		}

		MsgTracker *& track = pc->cr.trackers[f->__channel()];
		if( track == 0 ) {
			ptrace( cout << "..creating MsgTracker for channel " << f->__channel() << endl; )
			track = new MsgTracker();
		}

		if( f->op != F::NORMAL ) {
			if( f->__isACK() ) { 
				gotACK(f, pc, fromAddr);
				delete f;
				continue;
			}
			if( f->__isMISSING() ) { 
				gotMISSING(f, pc, fromAddr);
				delete f;
				continue;
			}
			if( f->op == F::RESET ) { 
				ptrace( cout << ".got RESET" << endl; )
				track->reset();
				pc->cs.resetIt();
				delete f;
				continue;
			}
		}

		if( track->recentlyReceived.count(f->__msgid()) ) { 
			// already done with it.  ignore, other than acking.
			if( f->__isREQUESTACK() )
				__sendACK(pc, fromAddr, f->__msgid());
			else { 
				ptrace( cout << ".ignoring packet about msg already received msg:" << f->__msgid() << " op:" << f->op << endl; )
			}
			delete f;
			continue;
		}

		if( f->__msgid() <= track->lastFullyReceived && !track->recentlyReceivedList.empty() ) {
			// reconnect on an old channel?
			ptrace( cout << ".warning: strange msgid:" << f->__msgid() << " received, last:" << track->lastFullyReceived << " conn:" << fromAddr.toString() << endl; )
		}

		MR *m = pc->cr.getPendingMsg(f, fromAddr); /* todo: optimize for single fragment case? */
		if( m == 0 ) { 
			ptrace( cout << "..getPendingMsg() returns 0" << endl; )
			delete f;
			continue;
		}
		if( m->got(f, fromAddr) ) {
			track->got(m->msgid);
			m->removeFromReceivingList();
			{
				lock lk(mutexr);
				pc->cr.received.push_back(m);
				pc->cr.receivedSome.notify_one();
			}
		}
	}
}

