/*
 * QDigiDocClient
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

#include "DigiDoc.h"

#include "Application.h"
#include "FileDialog.h"
#include "QSigner.h"

#include <common/Settings.h>
#include <common/SslCertificate.h>
#include <common/TokenData.h>

#include <digidocpp/DataFile.h>
#include <digidocpp/Signature.h>
#include <digidocpp/crypto/X509Cert.h>

#include <QDebug>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeData>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>
#include <QtGui/QPixmap>
#include <QtWidgets/QMessageBox>

#include <stdexcept>

using namespace digidoc;

static std::string to( const QString &str ) { return std::string( str.toUtf8().constData() ); }
static QString from( const std::string &str ) { return QString::fromUtf8( str.c_str() ).normalized( QString::NormalizationForm_C ); }
static QByteArray fromVector( const std::vector<unsigned char> &d )
{ return d.empty() ? QByteArray() : QByteArray( (const char *)d.data(), int(d.size()) ); }



DocumentModel::DocumentModel( DigiDoc *doc )
:	QAbstractTableModel( doc )
,	d( doc )
{
}

int DocumentModel::columnCount( const QModelIndex &parent ) const
{ return parent.isValid() ? 0 : NColumns; }

QString DocumentModel::save( const QModelIndex &index, const QString &path ) const
{
	if( !hasIndex( index.row(), index.column() ) )
		return QString();
	QFile::remove( path );
	d->b->dataFiles().at( index.row() )->saveAs( path.toUtf8().constData() );
	return path;
}

QVariant DocumentModel::data( const QModelIndex &index, int role ) const
{
	if( !hasIndex( index.row(), index.column() ) )
		return QVariant();

	const DataFile *file = d->b->dataFiles().at( index.row() );
	switch( role )
	{
	case Qt::ForegroundRole:
		switch( index.column() )
		{
		case Size: return QColor(Qt::gray);
		default: return QVariant();
		}
	case Qt::DisplayRole:
		switch( index.column() )
		{
		case Id: return QString::fromUtf8( file->id().c_str() );
		case Name: return from( file->fileName() );
		case Mime: return from( file->mediaType() );
		case Size: return FileDialog::fileSize( file->fileSize() );
		default: return QVariant();
		}
	case Qt::TextAlignmentRole:
		switch( index.column() )
		{
		case Name:
		case Mime: return int(Qt::AlignLeft|Qt::AlignVCenter);
		case Size: return int(Qt::AlignRight|Qt::AlignVCenter);
		default: return Qt::AlignCenter;
		}
	case Qt::ToolTipRole:
		switch( index.column() )
		{
		case Save: return tr("Save");
		case Remove: return tr("Remove");
		default: return tr("Filename: %1\nFilesize: %2\nMedia type: %3")
			.arg( from( file->fileName() ) )
			.arg( FileDialog::fileSize( file->fileSize() ) )
			.arg( from( file->mediaType() ) );
		}
	case Qt::DecorationRole:
		switch( index.column() )
		{
		case Save: return QPixmap(":/images/ico_save.png");
		case Remove: return QPixmap(":/images/ico_delete.png");
		default: return QVariant();
		}
	case Qt::SizeHintRole:
		switch( index.column() )
		{
		case Save:
		case Remove: return QSize( 20, 20 );
		default: return QVariant();
		}
	case Qt::UserRole:
		return FileDialog::safeName(from(file->fileName()));
	default: return QVariant();
	}
}

Qt::ItemFlags DocumentModel::flags( const QModelIndex & ) const
{ return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled; }

QMimeData* DocumentModel::mimeData( const QModelIndexList &indexes ) const
{
	QList<QUrl> list;
	for(const QModelIndex &index: indexes)
	{
		if( index.column() != 0 )
			continue;
		QString path = save( index, FileDialog::tempPath(index.data(Qt::UserRole).toString()) );
		if( !path.isEmpty() )
			list << QUrl::fromLocalFile( QFileInfo( path ).absoluteFilePath() );
	}
	QMimeData *data = new QMimeData();
	data->setUrls( list );
	return data;
}

QStringList DocumentModel::mimeTypes() const
{ return QStringList() << "text/uri-list"; }

void DocumentModel::open( const QModelIndex &index )
{
	QFileInfo f( save( index, FileDialog::tempPath(index.data(Qt::UserRole).toString()) ) );
	if( !f.exists() )
		return;
	d->m_tempFiles << f.absoluteFilePath();
#if defined(Q_OS_WIN)
	QStringList exts = QProcessEnvironment::systemEnvironment().value( "PATHEXT" ).split( ';' );
	exts << ".PIF" << ".SCR";
	if( exts.contains( "." + f.suffix(), Qt::CaseInsensitive ) &&
		QMessageBox::warning( qApp->activeWindow(), tr("DigiDoc3 client"),
			tr("This is an executable file! "
				"Executable files may contain viruses or other malicious code that could harm your computer. "
				"Are you sure you want to launch this file?"),
			QMessageBox::Yes|QMessageBox::No, QMessageBox::No ) == QMessageBox::No )
		return;
#else
	QFile::setPermissions( f.absoluteFilePath(), QFile::Permissions(0x6000) );
#endif
	QDesktopServices::openUrl( QUrl::fromLocalFile( f.absoluteFilePath() ) );
}

bool DocumentModel::removeRows( int row, int count, const QModelIndex &parent )
{
	if( !d->b || parent.isValid() )
		return false;

	try
	{
		beginRemoveRows( parent, row, row + count );
		for( int i = row + count - 1; i >= row; --i )
			d->b->removeDataFile( i );
		endRemoveRows();
		d->modified = true;
		return true;
	}
	catch( const Exception &e ) { d->setLastError( tr("Failed remove document from container"), e ); }
	return false;
}

void DocumentModel::reset()
{
	beginResetModel();
	endResetModel();
}

int DocumentModel::rowCount( const QModelIndex &parent ) const
{ return !d->b || parent.isValid() ? 0 : int(d->b->dataFiles().size()); }

Qt::DropActions DocumentModel::supportedDragActions() const
{
	return  Qt::CopyAction;
}



DigiDocSignature::DigiDocSignature(const digidoc::Signature *signature, const DigiDoc *parent)
:	s(signature)
,	m_parent(parent)
{}

QSslCertificate DigiDocSignature::cert() const
{
	try
	{
		return QSslCertificate( fromVector(s->signingCertificate()), QSsl::Der );
	}
	catch( const Exception & ) {}
	return QSslCertificate();
}

QDateTime DigiDocSignature::dateTime() const
{
	return toTime(s->trustedSigningTime());
}

QString DigiDocSignature::lastError() const { return m_lastError; }

QString DigiDocSignature::location() const
{
	QStringList l = locations();
	l.removeAll( "" );
	return l.join( ", " );
}

QStringList DigiDocSignature::locations() const
{
	return QStringList()
		<< from( s->city() ).trimmed()
		<< from( s->stateOrProvince() ).trimmed()
		<< from( s->postalCode() ).trimmed()
		<< from( s->countryName() ).trimmed();
}

QSslCertificate DigiDocSignature::ocspCert() const
{
	return QSslCertificate(
		fromVector(s->OCSPCertificate()), QSsl::Der );
}

QByteArray DigiDocSignature::ocspNonce() const
{
	return fromVector(s->OCSPNonce());
}

QDateTime DigiDocSignature::ocspTime() const
{
	return toTime(s->OCSPProducedAt());
}

const DigiDoc* DigiDocSignature::parent() const { return m_parent; }

void DigiDocSignature::parseException( DigiDocSignature::SignatureStatus &result, const digidoc::Exception &e ) const
{
	for(const Exception &child: e.causes())
	{
		switch( child.code() )
		{
		case Exception::ReferenceDigestWeak:
		case Exception::SignatureDigestWeak:
			m_warning |= DigestWeak;
			result = std::max( result, Warning );
			break;
		case Exception::DataFileNameSpaceWarning:
		case Exception::IssuerNameSpaceWarning:
		case Exception::ProducedATLateWarning:
		case Exception::MimeTypeWarning:
			result = std::max( result, Warning );
			break;
		case Exception::CertificateIssuerMissing:
		case Exception::CertificateUnknown:
		case Exception::OCSPResponderMissing:
		case Exception::OCSPCertMissing:
			result = std::max( result, Unknown );
			break;
		default:
			result = std::max( result, Invalid );
		}
		parseException( result, child );
	}
}

QString DigiDocSignature::policy() const
{
	return from(s->policy());
}

QString DigiDocSignature::profile() const
{
	return from(s->profile());
}

QString DigiDocSignature::role() const
{
	QStringList r = roles();
	r.removeAll( "" );
	return r.join( " / " );
}

QStringList DigiDocSignature::roles() const
{
	QStringList list;
	for(const std::string &role: s->signerRoles())
		list << from( role ).trimmed();
	return list;
}

void DigiDocSignature::setLastError( const Exception &e ) const
{
	QStringList causes;
	Exception::ExceptionCode code = Exception::General;
	DigiDoc::parseException(e, causes, code);
	m_lastError = causes.join( "\n" );
}

QString DigiDocSignature::signatureMethod() const
{ return from( s->signatureMethod() ); }

QString DigiDocSignature::signedBy() const
{
	return from(s->signedBy());
}

QDateTime DigiDocSignature::signTime() const
{
	return toTime(s->claimedSigningTime());
}

QString DigiDocSignature::spuri() const
{
	return from(s->SPUri());
}

QDateTime DigiDocSignature::toTime(const std::string &time) const
{
	QDateTime date;
	if(time.empty())
		return date;
	date = QDateTime::fromString(from(time), "yyyy-MM-dd'T'hh:mm:ss'Z'");
	date.setTimeSpec(Qt::UTC);
	return date;
}

QSslCertificate DigiDocSignature::tsCert() const
{
	return QSslCertificate(
		fromVector(s->TimeStampCertificate()), QSsl::Der );
}

QDateTime DigiDocSignature::tsTime() const
{
	return toTime(s->TimeStampTime());
}

QSslCertificate DigiDocSignature::tsaCert() const
{
	return QSslCertificate(
		fromVector(s->ArchiveTimeStampCertificate()), QSsl::Der );
}

QDateTime DigiDocSignature::tsaTime() const
{
	return toTime(s->ArchiveTimeStampTime());
}

DigiDocSignature::SignatureStatus DigiDocSignature::validate() const
{
	DigiDocSignature::SignatureStatus result = Valid;
	m_warning = 0;
	try
	{
		qApp->waitForTSL( m_parent->fileName() );
		s->validate();
	}
	catch( const Exception &e )
	{
		parseException( result, e );
		setLastError( e );
	}
	switch( result )
	{
	case Unknown:
		if ( validate(digidoc::Signature::POLv1) == Valid )
			return NonQSCD;
		return result;
	case Invalid: return result;
	default:
		if( SslCertificate( cert() ).type() & SslCertificate::TestType ||
			SslCertificate( ocspCert() ).type() & SslCertificate::TestType )
			return Test;

		return result;
	}
}

DigiDocSignature::SignatureStatus DigiDocSignature::validate(const std::string &policy) const
{
	DigiDocSignature::SignatureStatus result = Valid;
	try
	{
		s->validate(policy);
	}
	catch( const Exception &e )
	{
		parseException( result, e );
	}

	return result;
}

int DigiDocSignature::warning() const
{
	return m_warning;
}



DigiDoc::DigiDoc( QObject *parent )
:	QObject( parent )
,	m_documentModel( new DocumentModel( this ) )
{}

DigiDoc::~DigiDoc() { clear(); }

void DigiDoc::addFile( const QString &file )
{
	if( !checkDoc( b->signatures().size() > 0, tr("Cannot add files to signed container") ) )
		return;
	try {
		qDebug() << "DDOC add file:" << file.toLatin1().toHex() << to(file).c_str();
		b->addDataFile( to(file), "application/octet-stream" );
		m_documentModel->reset();
		modified = true;
	}
	catch( const Exception &e ) { setLastError( tr("Failed add file to container"), e ); }
}

bool DigiDoc::addSignature( const QByteArray &signature )
{
	if( !checkDoc( b->dataFiles().size() == 0, tr("Cannot add signature to empty container") ) )
		return false;

	try
	{
		b->addAdESSignature( std::vector<unsigned char>( signature.constData(), signature.constData() + signature.size() ) );
		modified = true;
		return true;
	}
	catch( const Exception &e ) { setLastError( tr("Failed to sign container"), e ); }
	return false;
}

bool DigiDoc::checkDoc( bool status, const QString &msg ) const
{
	if( isNull() )
		qApp->showWarning( tr("Container is not open") );
	else if( status )
		qApp->showWarning( msg );
	return !isNull() && !status;
}

void DigiDoc::clear()
{
	delete b;
	b = nullptr;
	delete parentContainer;
	parentContainer = nullptr;
	m_fileName.clear();
	m_documentModel->reset();
	for(const QString &file: m_tempFiles)
		QFile::remove(file);
	m_tempFiles.clear();
	modified = false;
}

void DigiDoc::create( const QString &file )
{
	clear();
	b = Container::create( to( file ) );
	m_fileName = file;
	m_documentModel->reset();
	modified = false;
}

DocumentModel* DigiDoc::documentModel() const { return m_documentModel; }

QString DigiDoc::fileName() const { return m_fileName; }
bool DigiDoc::isService() const
{
	return b->mediaType() == "application/pdf";
}
bool DigiDoc::isModified() const { return modified; }
bool DigiDoc::isNull() const { return b == nullptr; }
bool DigiDoc::isReadOnlyTS() const
{
    return b->mediaType() == "application/vnd.etsi.asic-s+zip";
}
bool DigiDoc::isSupported() const
{
	return b->mediaType() == "application/vnd.etsi.asic-e+zip";
}

QString DigiDoc::mediaType() const
{ return b ? from( b->mediaType() ) : QString(); }

QString DigiDoc::newSignatureID() const
{
	QStringList list;
	for(const Signature *s: b->signatures())
		list << QString::fromUtf8(s->id().c_str());
	unsigned int id = 0;
	while(list.contains(QString("S%1").arg(id), Qt::CaseInsensitive)) ++id;
	return QString("S%1").arg(id);
}

bool DigiDoc::open( const QString &file )
{
	qApp->waitForTSL( file );
	clear();
	try
	{
		b = Container::open( to(file) );
		QWidget *w = qobject_cast<QWidget*>(parent());
		if(isService())
		{
			QMessageBox::warning(w, w ? w->windowTitle() : 0,
				QCoreApplication::translate("SignatureDialog",
					"The verification of digital signatures in PDF format is performed through an external service. "
					"The file requiring verification will be forwarded to the service.\n"
					"The Information System Authority does not retain information regarding the files and users of the service."), QMessageBox::Ok);
		}
		else if(isReadOnlyTS())
		{
			const DataFile *f = b->dataFiles().at(0);
			if(QFileInfo(from(f->fileName())).suffix().toLower() == "ddoc")
			{
				const QString tmppath = FileDialog::tempPath(from(f->fileName()));
				f->saveAs(to(tmppath));
				QFileInfo f(tmppath);
				if(f.exists(tmppath))
				{
					m_tempFiles << tmppath;
					parentContainer = b;
					b = nullptr;
					b = Container::open(to(tmppath));
				}
			}
		}
		m_fileName = file;
		m_documentModel->reset();
		qApp->addRecent( file );
		modified = false;
		return true;
	}
	catch( const Exception &e )
	{ setLastError( tr("An error occurred while opening the document."), e ); }
	return false;
}

bool DigiDoc::parseException(const Exception &e, QStringList &causes, Exception::ExceptionCode &code)
{
	causes << QString( "%1:%2 %3").arg( QFileInfo(from(e.file())).fileName() ).arg( e.line() ).arg( from(e.msg()) );
	switch( e.code() )
	{
	case Exception::CertificateRevoked:
	case Exception::CertificateUnknown:
	case Exception::OCSPTimeSlot:
	case Exception::OCSPRequestUnauthorized:
	case Exception::PINCanceled:
	case Exception::PINFailed:
	case Exception::PINIncorrect:
	case Exception::PINLocked:
		code = e.code();
	default: break;
	}
	for(const Exception &c: e.causes())
		if(!parseException(c, causes, code))
			return false;
	return true;
}

void DigiDoc::removeSignature( unsigned int num )
{
	if( !checkDoc( num >= b->signatures().size(), tr("Missing signature") ) )
		return;
	try {
		b->removeSignature( num );
		modified = true;
	}
	catch( const Exception &e ) { setLastError( tr("Failed remove signature from container"), e ); }
}

void DigiDoc::save( const QString &filename )
{
	/*if( !checkDoc() );
		return; */
	try
	{
		if( !filename.isEmpty() )
			m_fileName = filename;
		b->save( to(m_fileName) );
		qApp->addRecent( filename );
		modified = false;
	}
	catch( const Exception &e ) { setLastError( tr("Failed to save container"), e ); }
}

void DigiDoc::setLastError( const QString &msg, const Exception &e )
{
	QStringList causes;
	Exception::ExceptionCode code = Exception::General;
	parseException(e, causes, code);
	switch( code )
	{
	case Exception::CertificateRevoked:
		qApp->showWarning(tr("Certificate status revoked"), causes.join("\n")); break;
	case Exception::CertificateUnknown:
		qApp->showWarning(tr("Certificate status unknown"), causes.join("\n")); break;
	case Exception::OCSPTimeSlot:
		qApp->showWarning(tr("Check your computer time"), causes.join("\n")); break;
	case Exception::OCSPRequestUnauthorized:
		qApp->showWarning(tr("You have not granted IP-based access. "
			"Check the settings of your server access certificate."), causes.join("\n")); break;
	case Exception::PINCanceled:
		break;
	case Exception::PINFailed:
		qApp->showWarning(tr("PIN Login failed"), causes.join("\n")); break;
	case Exception::PINIncorrect:
		qApp->showWarning(tr("PIN Incorrect"), causes.join("\n")); break;
	case Exception::PINLocked:
		qApp->showWarning(tr("PIN Locked. Please use ID-card utility for PIN opening!"), causes.join("\n")); break;
	default:
		qApp->showWarning(msg, causes.join("\n")); break;
	}
}

bool DigiDoc::sign( const QString &city, const QString &state, const QString &zip,
	const QString &country, const QString &role, const QString &role2 )
{
	if( !checkDoc( b->dataFiles().size() == 0, tr("Cannot add signature to empty container") ) )
		return false;

	try
	{
		qApp->signer()->setSignatureProductionPlace(
			to(city), to(state), to(zip), to(country) );
		std::vector<std::string> roles;
		if( !role.isEmpty() || !role2.isEmpty() )
			roles.push_back( to((QStringList() << role << role2).join(" / ")) );
		qApp->signer()->setSignerRoles( roles );
		qApp->signer()->setProfile( signatureFormat() == "LT" ? "time-stamp" : "time-mark" );
		qApp->waitForTSL( fileName() );
		b->sign( qApp->signer() );
		modified = true;
		return true;
	}
	catch( const Exception &e )
	{
		QStringList causes;
		Exception::ExceptionCode code = Exception::General;
		parseException(e, causes, code);
		if( code == Exception::PINIncorrect )
		{
			qApp->showWarning( tr("PIN Incorrect") );
			if( !(qApp->signer()->tokensign().flags() & TokenData::PinLocked) )
				return sign( city, state, zip, country, role, role2 );
		}
		else
			setLastError( tr("Failed to sign container"), e );
	}
	return false;
}

QString DigiDoc::signatureFormat() const
{
	if(m_fileName.endsWith("ddoc", Qt::CaseInsensitive))
		return "LT_TM";

	QString def = QFileInfo(m_fileName).suffix().compare("bdoc", Qt::CaseInsensitive) == 0 ? "LT_TM" : "LT";
	switch(b->signatures().size())
	{
	case 0:
		return def;
	case 1:
		return b->signatures()[0]->profile().find("time-stamp") != std::string::npos ? "LT" : "LT_TM";
	default: break;
	}
	Signature *sig = nullptr;
	for(Signature *s: b->signatures())
	{
		if(!sig)
			sig = s;
		else if(sig->profile() != s->profile())
			return def;
	}
	return sig->profile().find("time-stamp") != std::string::npos ? "LT" : "LT_TM";
}

QList<DigiDocSignature> DigiDoc::signatures() const
{
	QList<DigiDocSignature> list;
	if( isNull() )
		return list;
	for(const Signature *signature: b->signatures())
		list << DigiDocSignature(signature, this);
	return list;
}

QList<DigiDocSignature> DigiDoc::timestamps() const
{
	QList<DigiDocSignature> list;
	if(!parentContainer)
		return list;
	for(const Signature *signature: parentContainer->signatures())
		list << DigiDocSignature(signature, this);
	return list;
}

DigiDoc::DocumentType DigiDoc::documentType() const
{
	return checkDoc() && b->mediaType() == "application/vnd.etsi.asic-e+zip" ? BDoc2Type : DDocType;
}

QByteArray DigiDoc::getFileDigest( unsigned int i ) const
{
	if( !checkDoc() || i >= b->dataFiles().size() )
		return QByteArray();

	try
	{
		const DataFile *file = b->dataFiles().at( i );
		return fromVector(file->calcDigest("http://www.w3.org/2001/04/xmlenc#sha256"));
	}
	catch( const Exception & ) {}

	return QByteArray();
}
