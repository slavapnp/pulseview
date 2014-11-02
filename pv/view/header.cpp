/*
 * This file is part of the PulseView project.
 *
 * Copyright (C) 2012 Joel Holdsworth <joel@airwebreathe.org.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include "header.h"
#include "view.h"

#include "signal.h"
#include "../sigsession.h"

#include <cassert>
#include <algorithm>

#include <QApplication>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRect>

#include <pv/widgets/popup.h>

using std::max;
using std::make_pair;
using std::pair;
using std::shared_ptr;
using std::stable_sort;
using std::vector;

namespace pv {
namespace view {

const int Header::Padding = 12;
const int Header::BaselineOffset = 5;

Header::Header(View &parent) :
	MarginWidget(parent),
	_dragging(false)
{
	setFocusPolicy(Qt::ClickFocus);
	setMouseTracking(true);

	connect(&_view, SIGNAL(signals_moved()),
		this, SLOT(on_signals_moved()));
}

QSize Header::sizeHint() const
{
	QRectF max_rect(-Padding, 0, Padding, 0);
	for (auto &i : _view)
		if (i->enabled())
			max_rect = max_rect.united(i->label_rect(0));
	return QSize(max_rect.width() + Padding + BaselineOffset, 0);
}

shared_ptr<RowItem> Header::get_mouse_over_row_item(const QPoint &pt)
{
	const int w = width() - BaselineOffset;
	for (auto &i : _view)
		if (i->enabled() && i->label_rect(w).contains(pt))
			return i;
	return shared_ptr<RowItem>();
}

void Header::clear_selection()
{
	for (auto &i : _view)
		i->select(false);
	update();
}

void Header::signals_updated()
{
	for (shared_ptr<RowItem> r : _view) {
		assert(r);
		connect(r.get(), SIGNAL(appearance_changed()),
			this, SLOT(on_trace_changed()));
	}
}

void Header::show_popup(const shared_ptr<RowItem> &item)
{
	using pv::widgets::Popup;

	Popup *const p = item->create_popup(&_view);
	if (!p)
		return;

	const QPoint pt(width() - BaselineOffset, item->get_y());
	p->set_position(mapToGlobal(pt), Popup::Right);
	p->show();
}

void Header::paintEvent(QPaintEvent*)
{
	// The trace labels are not drawn with the arrows exactly on the
	// left edge of the widget, because then the selection shadow
	// would be clipped away.
	const int w = width() - BaselineOffset;

	vector< shared_ptr<RowItem> > row_items(
		_view.begin(), _view.end());

	stable_sort(row_items.begin(), row_items.end(),
		[](const shared_ptr<RowItem> &a, const shared_ptr<RowItem> &b) {
			return a->v_offset() < b->v_offset(); });

	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);

	for (const shared_ptr<RowItem> r : row_items)
	{
		assert(r);

		const bool highlight = !_dragging &&
			r->label_rect(w).contains(_mouse_point);
		r->paint_label(painter, w, highlight);
	}

	painter.end();
}

void Header::mouseLeftPressEvent(QMouseEvent *event)
{
	(void)event;

	const bool ctrl_pressed =
		QApplication::keyboardModifiers() & Qt::ControlModifier;

	// Clear selection if control is not pressed and this item is unselected
	if ((!_mouse_down_item || !_mouse_down_item->selected()) &&
		!ctrl_pressed)
		for (shared_ptr<RowItem> r : _view)
			r->select(false);

	// Set the signal selection state if the item has been clicked
	if (_mouse_down_item) {
		if (ctrl_pressed)
			_mouse_down_item->select(!_mouse_down_item->selected());
		else
			_mouse_down_item->select(true);
	}

	// Save the offsets of any signals which will be dragged
	for (const shared_ptr<RowItem> r : _view)
		if (r->selected())
			r->drag();

	selection_changed();
	update();
}

void Header::mousePressEvent(QMouseEvent *event)
{
	assert(event);

	_mouse_down_point = event->pos();
	_mouse_down_item = get_mouse_over_row_item(event->pos());

	if (event->button() & Qt::LeftButton)
		mouseLeftPressEvent(event);
}

void Header::mouseLeftReleaseEvent(QMouseEvent *event)
{
	assert(event);

	const bool ctrl_pressed =
		QApplication::keyboardModifiers() & Qt::ControlModifier;

	// Unselect everything if control is not pressed
	const shared_ptr<RowItem> mouse_over =
		get_mouse_over_row_item(event->pos());

	for (auto &r : _view)
		r->drag_release();

	if (!_dragging)
	{
		if (!ctrl_pressed) {
			for (shared_ptr<RowItem> r : _view)
				if (_mouse_down_item != r)
					r->select(false);

			if (_mouse_down_item)
				show_popup(_mouse_down_item);
		}
	}

	_dragging = false;
}

void Header::mouseReleaseEvent(QMouseEvent *event)
{
	assert(event);
	if (event->button() & Qt::LeftButton)
		mouseLeftReleaseEvent(event);

	_mouse_down_item = nullptr;
}

void Header::mouseMoveEvent(QMouseEvent *event)
{
	assert(event);
	_mouse_point = event->pos();

	if (!(event->buttons() & Qt::LeftButton))
		return;

	if ((event->pos() - _mouse_down_point).manhattanLength() <
		QApplication::startDragDistance())
		return;

	// Check all the drag items share a common owner
	RowItemOwner *item_owner = nullptr;
	for (shared_ptr<RowItem> r : _view)
		if (r->dragging()) {
			if (!item_owner)
				item_owner = r->owner();
			else if(item_owner != r->owner())
				return;
		}

	if (!item_owner)
		return;

	// Do the drag
	_dragging = true;

	const int delta = event->pos().y() - _mouse_down_point.y();

	for (std::shared_ptr<RowItem> r : _view)
		if (r->dragging()) {
			r->set_v_offset(r->drag_point().y() + delta);

			// Ensure the trace is selected
			r->select();
		}

	signals_moved();

	update();
}

void Header::leaveEvent(QEvent*)
{
	_mouse_point = QPoint(-1, -1);
	update();
}

void Header::contextMenuEvent(QContextMenuEvent *event)
{
	const shared_ptr<RowItem> r = get_mouse_over_row_item(_mouse_point);
	if (!r)
		return;

	QMenu *const menu = r->create_context_menu(this);
	if (!menu)
		return;

	menu->exec(event->globalPos());
}

void Header::keyPressEvent(QKeyEvent *e)
{
	assert(e);

	switch (e->key())
	{
	case Qt::Key_Delete:
	{
		for (const shared_ptr<RowItem> r : _view)
			if (r->selected())
				r->delete_pressed();
		break;
	}
	}
}

void Header::on_signals_moved()
{
	update();
}

void Header::on_trace_changed()
{
	update();
	geometry_updated();
}

} // namespace view
} // namespace pv
