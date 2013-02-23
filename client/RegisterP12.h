/*
 * QDigiDocClient
 *
 * Copyright (C) 2009-2013 Jargo Kõster <jargo@innovaatik.ee>
 * Copyright (C) 2009-2013 Raul Metsma <raul@innovaatik.ee>
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

#pragma once

#include <QtCore/QtGlobal>
#if QT_VERSION >= 0x050000
#include <QtWidgets/QDialog>
#else
#include <QtGui/QDialog>
#endif

namespace Ui { class RegisterP12; }

class RegisterP12: public QDialog
{
	Q_OBJECT
public:
	explicit RegisterP12( QWidget *parent = 0 );
	explicit RegisterP12( const QString &cert, QWidget *parent = 0 );
	~RegisterP12();

private Q_SLOTS:
	void on_buttonBox_accepted();
	void on_showP12Cert_clicked();
	void on_p12Button_clicked();
	void on_p12Cert_textChanged( const QString &text );
	void on_p12Pass_textChanged( const QString &text );

private:
	bool eventFilter( QObject *o, QEvent *e );
	void validateP12Cert();

	Ui::RegisterP12 *d;
};
