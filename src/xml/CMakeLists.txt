# SPDX-License-Identifier: GPL-2.0-or-later

target_sources(inkscape_base PRIVATE
	composite-node-observer.cpp
	croco-node-iface.cpp
	event.cpp
	log-builder.cpp
	node-fns.cpp
	node.cpp
	node-iterators.cpp
	quote.cpp
	repr.cpp
	repr-css.cpp
	repr-io.cpp
	repr-sorting.cpp
	repr-util.cpp
	simple-document.cpp
	simple-node.cpp
	subtree.cpp
	helper-observer.cpp
	rebase-hrefs.cpp
	href-attribute-helper.cpp

	# Headers
	attribute-record.h
	comment-node.h
	composite-node-observer.h
	croco-node-iface.h
	document.h
	element-node.h
	event-fns.h
	event.h
	helper-observer.h
	invalid-operation-exception.h
	log-builder.h
	node-fns.h
	node-iterators.h
	node-observer.h
	node.h
	pi-node.h
	quote-test.h
	quote.h
	rebase-hrefs.h
	repr-action-test.h
	proof-ns.h
	repr-sorting.h
	repr.h
	simple-document.h
	simple-node.h
	sp-css-attr.h
	subtree.h
	text-node.h
	href-attribute-helper.h
)
