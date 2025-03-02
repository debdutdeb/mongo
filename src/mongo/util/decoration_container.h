/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <cstdint>
#include <memory>

namespace mongo {

template <typename DecoratedType>
class DecorationRegistry;

template <typename DecoratedType>
class Decorable;

/**
 * An container for decorations.
 */
template <typename DecoratedType>
class DecorationContainer {
    DecorationContainer(const DecorationContainer&) = delete;
    DecorationContainer& operator=(const DecorationContainer&) = delete;

public:
    /**
     * Opaque descriptor of a decoration.  It is an identifier to a field on the
     * DecorationContainer that is private to those modules that have access to the descriptor.
     */
    class DecorationDescriptor {
    public:
        DecorationDescriptor() = default;

    private:
        friend DecorationContainer;
        friend DecorationRegistry<DecoratedType>;
        friend Decorable<DecoratedType>;

        explicit DecorationDescriptor(size_t index) : _index(index) {}

        size_t _index;
    };

    /**
     * Opaque description of a decoration of specified type T.  It is an identifier to a field
     * on the DecorationContainer that is private to those modules that have access to the
     * descriptor.
     */
    template <typename T>
    class DecorationDescriptorWithType {
    public:
        DecorationDescriptorWithType() = default;

    private:
        friend DecorationContainer;
        friend DecorationRegistry<DecoratedType>;
        friend Decorable<DecoratedType>;

        explicit DecorationDescriptorWithType(DecorationDescriptor raw) : _raw(std::move(raw)) {}

        DecorationDescriptor _raw;
    };

    /**
     * Constructs a decorable built based on the given "registry."
     *
     * The registry must stay in scope for the lifetime of the DecorationContainer, and must not
     * have any declareDecoration() calls made on it while a DecorationContainer dependent on it
     * is in scope.
     */
    explicit DecorationContainer(Decorable<DecoratedType>* const decorated,
                                 const DecorationRegistry<DecoratedType>* const registry)
        : _registry(registry) {
        // Because the decorations live in the externally allocated storage buffer at
        // `_decorationData`, there needs to be a way to get back from a known location within this
        // buffer to the type which owns those decorations.  We place a pointer to ourselves, a
        // "back link" in the front of this storage buffer, as this is the easiest "well known
        // location" to compute.
        auto const backLink = reinterpret_cast<Decorable<DecoratedType>**>(_decorationData.get());
        *backLink = decorated;
        _registry->construct(this);
    }

    /**
     * Constructs a copyable decorable built based on the given "registry."
     *
     * All decorations are copy constructed from provided DecorationContainer.
     */
    explicit DecorationContainer(Decorable<DecoratedType>* const decorated,
                                 const DecorationRegistry<DecoratedType>* const registry,
                                 const DecorationContainer& other)
        : _registry(registry) {
        // Because the decorations live in the externally allocated storage buffer at
        // `_decorationData`, there needs to be a way to get back from a known location within this
        // buffer to the type which owns those decorations.  We place a pointer to ourselves, a
        // "back link" in the front of this storage buffer, as this is the easiest "well known
        // location" to compute.
        auto const backLink = reinterpret_cast<Decorable<DecoratedType>**>(_decorationData.get());
        *backLink = decorated;
        _registry->copyConstruct(this, &other);
    }

    ~DecorationContainer() {
        _registry->destroy(this);
    }

    /**
     * Gets the decorated value for the given descriptor.
     *
     * The descriptor must be one returned from this DecorationContainer's associated _registry.
     */
    void* getDecoration(DecorationDescriptor descriptor) {
        return _decorationData.get() + descriptor._index;
    }

    /**
     * Same as the non-const form above, but returns a const result.
     */
    const void* getDecoration(DecorationDescriptor descriptor) const {
        return _decorationData.get() + descriptor._index;
    }

    /**
     * Gets the decorated value or the given typed descriptor.
     */
    template <typename T>
    T& getDecoration(DecorationDescriptorWithType<T> descriptor) {
        return *static_cast<T*>(getDecoration(descriptor._raw));
    }

    /**
     * Same as the non-const form above, but returns a const result.
     */
    template <typename T>
    const T& getDecoration(DecorationDescriptorWithType<T> descriptor) const {
        return *static_cast<const T*>(getDecoration(descriptor._raw));
    }

private:
    const DecorationRegistry<DecoratedType>* const _registry;
    const std::unique_ptr<unsigned char[]> _decorationData =
        std::make_unique<unsigned char[]>(_registry->getDecorationBufferSizeBytes());
};

}  // namespace mongo
