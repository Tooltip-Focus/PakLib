// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gaëtan Dezeiraud

#include "pak/Reader.h"

#include <utility>

namespace pak
{
	MappedView::MappedView(std::shared_ptr<const detail::ArchiveState> state, const std::byte *data, std::size_t size) noexcept
	    : m_state(std::move(state))
	    , m_data(data)
	    , m_size(size)
	{
	}

	MappedView::MappedView(MappedView &&other) noexcept
	    : m_state(std::move(other.m_state))
	    , m_data(std::exchange(other.m_data, nullptr))
	    , m_size(std::exchange(other.m_size, 0))
	{
	}

	MappedView &MappedView::operator=(MappedView &&other) noexcept
	{
		if(this != &other)
		{
			m_state = std::move(other.m_state);
			m_data  = std::exchange(other.m_data, nullptr);
			m_size  = std::exchange(other.m_size, 0);
		}
		return *this;
	}

	MappedView::~MappedView() noexcept = default;

	std::span<const std::byte> MappedView::Bytes() const noexcept
	{
		return {m_data, m_size};
	}

	MappedView::operator bool() const noexcept
	{
		return m_state != nullptr || m_size == 0;
	}
}   // namespace pak
