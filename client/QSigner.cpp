/*
 * QDigiDocClient
 *
 * Copyright (C) 2009,2010 Jargo Kõster <jargo@innovaatik.ee>
 * Copyright (C) 2009,2010 Raul Metsma <raul@innovaatik.ee>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "QSigner.h"

#include <common/PinDialog.h>
#include <common/SslCertificate.h>
#include <common/TokenData.h>

#include <digidocpp/Conf.h>

#include <libp11.h>

#include <QApplication>
#include <QMutex>
#include <QStringList>

#define CKR_OK					(0)
#define CKR_CANCEL				(1)
#define CKR_FUNCTION_CANCELED	(0x50)
#define CKR_PIN_INCORRECT		(0xa0)
#define CKR_PIN_LOCKED			(0xa4)

class QSignerPrivate
{
public:
	QSignerPrivate(): flags(0), login(false), terminate(false), handle(0), slot(0), slotCount(0), loginResult(CKR_OK) {}

	QSslCertificate	cert;
	TokenData::TokenFlags flags;
	volatile bool	login, terminate;
	QMutex			m;
	QHash<QString,unsigned int> cards;
	QString			selectedCard, select;
	PKCS11_CTX		*handle;
	PKCS11_SLOT     *slot, *slots;
	unsigned int	slotCount;
	unsigned long	loginResult;
};



using namespace digidoc;

QSigner::QSigner( QObject *parent )
:	QThread( parent )
,	d( new QSignerPrivate )
{}

QSigner::~QSigner() { unloadDriver(); delete d; }

X509* QSigner::getCert() throw(digidoc::SignException)
{
	if( d->cert.isNull() )
		throw SignException( __FILE__, __LINE__, tr("Sign certificate is not selected").toUtf8().constData() );
	return (X509*)d->cert.handle();
}

void QSigner::emitDataChanged()
{
	TokenData data;
	data.setCard( d->selectedCard );
	data.setCards( d->cards.keys() );
	data.setCert( d->cert );
	data.setFlags( d->flags );
	Q_EMIT dataChanged( data );
}

bool QSigner::loadDriver()
{
	std::string name;
	try
	{
		name = Conf::getInstance()->getPKCS11DriverPath();
	}
	catch( const Exception & )
	{
		Q_EMIT error( tr("Failed to load PKCS#11 module") );
		return false;
	}

	if( !d->handle &&
		(!(d->handle = PKCS11_CTX_new()) || PKCS11_CTX_load( d->handle, name.c_str() )) )
	{
		PKCS11_CTX_free( d->handle );
		d->handle = 0;
		return false;
	}
	if( !isRunning() )
		start();
	return true;
}

void QSigner::lock() { d->m.lock(); }

void QSigner::read()
{
	d->cards.clear();
	if( d->slotCount )
	{
		PKCS11_release_all_slots( d->handle, d->slots, d->slotCount );
		d->slotCount = 0;
	}

	if( PKCS11_enumerate_slots( d->handle, &d->slots, &d->slotCount ) )
	{
		d->cert = QSslCertificate();
		d->selectedCard.clear();
		d->flags = 0;
		emitDataChanged();
		return;
	}

	for( unsigned int i = 0; i < d->slotCount; ++i )
	{
		PKCS11_SLOT* slot = &d->slots[i];
		if( !slot->token )
			continue;

		QString serialNumber = QByteArray( (const char*)slot->token->serialnr, 16 ).trimmed();
		if( !d->cards.contains( serialNumber ) )
			d->cards[serialNumber] = i + 1;
	}

	if( !d->selectedCard.isEmpty() && !d->cards.contains( d->selectedCard ) )
	{
		d->cert = QSslCertificate();
		d->selectedCard.clear();
		d->flags = 0;
	}
	emitDataChanged();

	if( d->selectedCard.isEmpty() && !d->cards.isEmpty() )
		selectCert( d->cards.keys().first() );
}

void QSigner::run()
{
	d->terminate = false;

	d->cards["loading"] = 0;
	d->selectedCard = "loading";
	d->flags = 0;
	d->cert = QSslCertificate();
	emitDataChanged();

	if( !loadDriver() )
	{
		Q_EMIT error( tr("Failed to load PKCS#11 module") );
		return;
	}

	while( !d->terminate )
	{
		if( d->m.tryLock() )
		{
			read();
			if( !d->select.isEmpty() && d->cards.contains( d->select ) )
				selectCert( d->select );
			d->select.clear();
			d->m.unlock();
		}

		if( d->login )
		{
			d->loginResult = CKR_OK;
			if( PKCS11_login( d->slot, 0, NULL ) < 0 )
				d->loginResult = ERR_get_error();
			d->login = false;
		}

		sleep( 1 );
	}
}

void QSigner::selectCard( const QString &card ) { d->select = card; }

void QSigner::selectCert( const QString &card )
{
	d->selectedCard = card;
	d->slot = 0;
	d->flags = 0;
	d->cert = QSslCertificate();
	emitDataChanged();

	if( d->slotCount )
	{
		PKCS11_release_all_slots( d->handle, d->slots, d->slotCount );
		d->slotCount = 0;
	}
	if( PKCS11_enumerate_slots( d->handle, &d->slots, &d->slotCount ) ||
		d->cards[card] >= d->slotCount ||
		!(d->slot = &d->slots[d->cards[card]]) )
		return;

	for( unsigned int i = 0; i < d->slotCount; ++i )
	{
		PKCS11_SLOT *slot = &(d->slots[i]);
		PKCS11_CERT *certs;
		unsigned int numberOfCerts;
		if( !slot->token ||
			d->selectedCard != QByteArray( (const char*)slot->token->serialnr, 16 ).trimmed() ||
			PKCS11_enumerate_certs( slot->token, &certs, &numberOfCerts ) ||
			numberOfCerts <= 0 )
			continue;

		SslCertificate cert = SslCertificate::fromX509( Qt::HANDLE((&certs[0])->x509) );
		if( cert.keyUsage().keys().contains( SslCertificate::NonRepudiation ) )
		{
			d->slot = slot;
			d->cert = cert;
			d->cards[d->selectedCard] = i;
#ifdef LIBP11_TOKEN_FLAGS
			if( d->slot->token->userPinCountLow || d->slot->token->soPinCountLow )
				d->flags |= TokenData::PinCountLow;
			if( d->slot->token->userPinFinalTry || d->slot->token->soPinFinalTry )
				d->flags |= TokenData::PinFinalTry;
			if( d->slot->token->userPinLocked || d->slot->token->soPinLocked )
				d->flags |= TokenData::PinLocked;
#endif
			break;
		}
	}
	emitDataChanged();
}

void QSigner::sign( const Digest &digest, Signature &signature ) throw(digidoc::SignException)
{
	QMutexLocker locker( &d->m );
	if( !d->cards.contains( d->selectedCard ) || d->cert.isNull() )
		throwException( tr("Signing certificate is not selected."), 0, Exception::NoException, __LINE__ );

	selectCert( d->selectedCard );
	if( !d->slot || !d->slot->token )
		throwException( tr("Failed to login token"), ERR_get_error(), Exception::NoException, __LINE__ );

	// HACK: clean Fedora 13 ssl error queue
	// ERROR: 185073780 - error:0B080074:x509 certificate routines:X509_check_private_key:key values mismatch
	ERR_get_error();
	if( d->slot->token->loginRequired )
	{
		unsigned long err = CKR_OK;
		if( d->slot->token->secureLogin )
		{
			d->login = true;
			PinDialog p( PinDialog::Pin2PinpadType, d->cert, d->flags, qApp->activeWindow() );
			p.open();
			do
			{
				wait( 1 );
				qApp->processEvents();
			} while( d->login );
			err = d->loginResult;
		}
		else
		{
			PinDialog p( PinDialog::Pin2Type, d->cert, d->flags, qApp->activeWindow() );
			if( !p.exec() )
				throwException( tr("PIN acquisition canceled."), 0, Exception::PINCanceled, __LINE__ );
			if( PKCS11_login( d->slot, 0, p.text().toUtf8() ) < 0 )
				err = ERR_get_error();
		}
		switch( ERR_GET_REASON(err) )
		{
		case CKR_OK: break;
		case CKR_CANCEL:
		case CKR_FUNCTION_CANCELED:
			throwException( tr("PIN acquisition canceled."), 0, Exception::PINCanceled, __LINE__ );
		case CKR_PIN_INCORRECT:
			selectCert( d->selectedCard );
			throwException( tr("PIN Incorrect"), 0, Exception::PINIncorrect, __LINE__ );
		case CKR_PIN_LOCKED:
			throwException( tr("PIN Locked"), 0, Exception::PINLocked, __LINE__ );
		default:
			throwException( tr("Failed to login token"), err, Exception::NoException, __LINE__ );
		}
	}

	PKCS11_CERT *certs;
	unsigned int certCount;
	if( PKCS11_enumerate_certs( d->slot->token, &certs, &certCount ) )
		throwException( tr("Failed to sign document"), ERR_get_error(), Exception::NoException, __LINE__ );
	if( !certCount || !&certs[0] )
		throwException( tr("Failed to sign document") + "\nNo sertificates", 0, Exception::NoException, __LINE__ );

	PKCS11_KEY *key = PKCS11_find_key( &certs[0] );
	if( !key )
		throwException( tr("Failed to sign document") + "\nNo keys", 0, Exception::NoException, __LINE__ );

	if( PKCS11_sign(digest.type, digest.digest, digest.length, signature.signature, &(signature.length), key) != 1 )
		throwException( tr("Failed to sign document"), ERR_get_error(), Exception::NoException, __LINE__ );
}

void QSigner::throwException( const QString &msg, unsigned long err, Exception::ExceptionCode code, int line ) throw(SignException)
{
	QString t = msg;
	if( err )
	{
		t += "\n";
		t += QString::fromUtf8( ERR_error_string( err, NULL ) );
	}
	SignException e( __FILE__, line, t.toUtf8().constData() );
	e.setCode( code );
	throw e;
}

void QSigner::unloadDriver()
{
	d->terminate = true;
	wait();
	if( d->slotCount )
		PKCS11_release_all_slots( d->handle, d->slots, d->slotCount );
	d->slotCount = 0;
	if( d->handle )
	{
		PKCS11_CTX_unload( d->handle );
		PKCS11_CTX_free( d->handle );
		d->handle = 0;
	}
}

void QSigner::unlock() { d->m.unlock(); }
