/*****************************************************************************
 * Copyright (C) 2024 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * ( at your option ) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

import QtQuick

// This item can be used as a layer effect.
// The purpose of this item is to apply an effect to a partial
// area of an item without re-rendering the source item multiple
// times.
Item {
    id: root

    // source must be a texture provider Item.
    // If layer is enabled, the source (parent) will be a texture provider
    // and automatically set by Qt.
    // Some items, such as Image and ShaderEffectSource can be implicitly
    // a texture provider without creating an extra layer.
    // Make sure that the sampler name is set to "source" (default) if
    // this is used as a layer effect.
    property Item source

    // Default layer properties are used except that `enabled` is set by default.
    // The geometry of the effect will be adjusted to `effectRect` automatically.
    property alias effectLayer: effectProxy.layer

    // Rectangular area where the effect should be applied:
    property alias effectRect: effectProxy.effectRect

    // Enable blending if background of source is not opaque.
    // This comes with a performance penalty.
    property alias blending: sourceProxy.blending

    // This item displays the item with the rect denoted by effectRect
    // being transparent (only when blending is set):
    ShaderEffect {
        id: sourceProxy

        anchors.fill: parent

        blending: false

        property alias source: root.source

        readonly property rect discardRect: {
            if (blending)
                return Qt.rect(effectProxy.x / root.width,
                               effectProxy.y / root.height,
                               (effectProxy.x + effectProxy.width) / root.width,
                               (effectProxy.y + effectProxy.height) / root.height)
            else // If blending is not enabled, no need to make the normalization calculations
                return Qt.rect(0, 0, 0, 0)
        }

        // cullMode: ShaderEffect.BackFaceCulling // QTBUG-136611 (Layering breaks culling with OpenGL)

        // Simple filter that is only enabled when blending is active.
        // We do not want the source to be rendered below the frosted glass effect.
        // NOTE: It might be a better idea to enable this at all times if texture sampling
        //       is costlier than branching.

        fragmentShader: blending ? "qrc:///shaders/RectFilter.frag.qsb" : ""
    }

    // This item represents the region where the effect is applied.
    // `effectLayer` only has access to the area denoted with
    // the property `effectRect`
    ShaderEffect {
        id: effectProxy

        x: effectRect.x
        y: effectRect.y
        width: effectRect.width
        height: effectRect.height

        blending: root.blending

        // This item is used to show effect
        // so it is pointless to show if
        // there is no effect to show.
        visible: layer.enabled

        // cullMode: ShaderEffect.BackFaceCulling // QTBUG-136611 (Layering breaks culling with OpenGL)

        property rect effectRect

        property alias source: root.source

        readonly property rect normalRect: Qt.rect(effectRect.x / root.width,
                                                   effectRect.y / root.height,
                                                   effectRect.width / root.width,
                                                   effectRect.height / root.height)

        vertexShader: "qrc:///shaders/SubTexture.vert.qsb"

        layer.enabled: layer.effect
    }
}
