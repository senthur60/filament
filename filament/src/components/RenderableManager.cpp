/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "FilamentAPI-impl.h"

#include "components/RenderableManager.h"

#include "details/Engine.h"
#include "details/VertexBuffer.h"
#include "details/IndexBuffer.h"
#include "details/Material.h"
#include "details/RenderPrimitive.h"

#include <utils/Log.h>
#include <utils/Panic.h>

using namespace math;
using namespace utils;

namespace filament {

using namespace details;

struct RenderableManager::BuilderDetails {
    using Entry = RenderableManager::Builder::Entry;
    Entry* mEntries = nullptr;
    size_t mEntriesCount = 0;
    Box mAABB;
    uint8_t mLayerMask = 0x1;
    uint8_t mPriority = 0x4;
    bool mCulling : 1;
    bool mCastShadows : 1;
    bool mReceiveShadows : 1;
    uint8_t mSkinningBoneCount = 0;
    Bone const* mBones = nullptr;
    math::mat4f const* mBoneMatrices = nullptr;

    explicit BuilderDetails(size_t count)
            : mEntriesCount(count), mCulling(true), mCastShadows(false), mReceiveShadows(true) {
    }
    // this is only needed for the explicit instantiation below
    BuilderDetails() = default;
};

using BuilderType = RenderableManager;
BuilderType::Builder::Builder(size_t count) noexcept
        : BuilderBase<RenderableManager::BuilderDetails>(count) {
    mImpl->mEntries = new Entry[count];
}
BuilderType::Builder::~Builder() noexcept {
    delete [] mImpl->mEntries;
}
BuilderType::Builder::Builder(BuilderType::Builder&& rhs) noexcept = default;
BuilderType::Builder& BuilderType::Builder::operator=(BuilderType::Builder&& rhs) noexcept = default;


RenderableManager::Builder& RenderableManager::Builder::geometry(size_t index,
        PrimitiveType type, VertexBuffer* vertices, IndexBuffer* indices) noexcept {
    return geometry(index, type, vertices, indices,
            0, 0, vertices->getVertexCount() - 1, indices->getIndexCount());
}

RenderableManager::Builder& RenderableManager::Builder::geometry(size_t index,
        PrimitiveType type, VertexBuffer* vertices, IndexBuffer* indices,
        size_t offset, size_t count) noexcept {
    return geometry(index, type, vertices, indices, offset,
            0, vertices->getVertexCount() - 1, count);
}

RenderableManager::Builder& RenderableManager::Builder::geometry(size_t index,
        PrimitiveType type, VertexBuffer* vertices, IndexBuffer* indices,
        size_t offset, size_t minIndex, size_t maxIndex, size_t count) noexcept {
    if (index < mImpl->mEntriesCount) {
        mImpl->mEntries[index].vertices = vertices;
        mImpl->mEntries[index].indices = indices;
        mImpl->mEntries[index].offset = offset;
        mImpl->mEntries[index].minIndex = minIndex;
        mImpl->mEntries[index].maxIndex = maxIndex;
        mImpl->mEntries[index].count = count;
        mImpl->mEntries[index].type = type;
    }
    return *this;
}

RenderableManager::Builder& RenderableManager::Builder::material(size_t index,
        MaterialInstance const* materialInstance) noexcept {
    if (index < mImpl->mEntriesCount) {
        mImpl->mEntries[index].materialInstance = materialInstance;
    }
    return *this;
}

RenderableManager::Builder& RenderableManager::Builder::boundingBox(const Box& axisAlignedBoundingBox) noexcept {
    mImpl->mAABB = axisAlignedBoundingBox;
    return *this;
}

RenderableManager::Builder& RenderableManager::Builder::layerMask(uint8_t select, uint8_t values) noexcept {
    mImpl->mLayerMask = (mImpl->mLayerMask & ~select) | (values & select);
    return *this;
}

RenderableManager::Builder& RenderableManager::Builder::priority(uint8_t priority) noexcept {
    mImpl->mPriority = std::min(priority, uint8_t(0x7));
    return *this;
}

RenderableManager::Builder& RenderableManager::Builder::culling(bool enable) noexcept {
    mImpl->mCulling = enable;
    return *this;
}

RenderableManager::Builder& RenderableManager::Builder::castShadows(bool enable) noexcept {
    mImpl->mCastShadows = enable;
    return *this;
}

RenderableManager::Builder& RenderableManager::Builder::receiveShadows(bool enable) noexcept {
    mImpl->mReceiveShadows = enable;
    return *this;
}

RenderableManager::Builder& RenderableManager::Builder::skinning(size_t boneCount) noexcept {
    mImpl->mSkinningBoneCount = (uint8_t)std::min(size_t(255), boneCount);
    return *this;
}

RenderableManager::Builder& RenderableManager::Builder::skinning(
        size_t boneCount, Bone const* transforms) noexcept {
    mImpl->mSkinningBoneCount = (uint8_t)std::min(size_t(255), boneCount);
    mImpl->mBones = transforms;
    return *this;
}

RenderableManager::Builder& RenderableManager::Builder::skinning(
        size_t boneCount, math::mat4f const* transforms) noexcept {
    mImpl->mSkinningBoneCount = (uint8_t)std::min(size_t(255), boneCount);
    mImpl->mBoneMatrices = transforms;
    return *this;
}

RenderableManager::Builder& RenderableManager::Builder::blendOrder(size_t index, uint16_t blendOrder) noexcept {
    if (index < mImpl->mEntriesCount) {
        mImpl->mEntries[index].blendOrder = blendOrder;
    }
    return *this;
}

RenderableManager::Builder::Result RenderableManager::Builder::build(Engine& engine, Entity entity) {
    bool isEmpty = true;
    for (size_t i = 0, c = mImpl->mEntriesCount; i < c; i++) {
        auto& entry = mImpl->mEntries[i];

        // entry.materialInstance must be set to something even if indices/vertices are null
        FMaterial const* material = nullptr;
        if (!entry.materialInstance) {
            material = upcast(engine.getDefaultMaterial());
            entry.materialInstance = material->getDefaultInstance();
        } else {
            material = upcast(entry.materialInstance->getMaterial());
        }

        // primitves without indices or vertices will be ignored
        if (!entry.indices || !entry.vertices) {
            continue;
        }

        // reject invalid geometry parameters
        if (!ASSERT_PRECONDITION_NON_FATAL(entry.offset + entry.count <= entry.indices->getIndexCount(),
                "[entity=%u, primitive @ %u] offset (%u) + count (%u) > indexCount (%u)",
                i, entity.getId(),
                entry.offset, entry.count, entry.indices->getIndexCount())) {
            entry.vertices = nullptr;
            return Error;
        }

        if (!ASSERT_PRECONDITION_NON_FATAL(entry.minIndex <= entry.maxIndex,
                "[entity=%u, primitive @ %u] minIndex (%u) > maxIndex (%u)",
                i, entity.getId(),
                entry.minIndex, entry.maxIndex)) {
            entry.vertices = nullptr;
            return Error;
        }

#ifndef NDEBUG
        // this can't be an error because (1) those values are not immutable, so the caller
        // could fix later, and (2) the material's shader will work (i.e. compile), and
        // use the default values for this attribute, which maybe be acceptable.
        AttributeBitset declared = upcast(entry.vertices)->getDeclaredAttributes();
        AttributeBitset required = material->getRequiredAttributes();
        if ((declared & required) != required) {
            slog.w << "[entity=" << entity.getId() << ", primitive @ " << i
                   << "] missing required attributes ("
                   << required << "), declared=" << declared << io::endl;
        }
#endif

        // we have at least one valid primitive
        isEmpty = false;
    }

    if (!ASSERT_POSTCONDITION_NON_FATAL(
            !mImpl->mAABB.isEmpty() ||
            (!mImpl->mCulling && (!(mImpl->mReceiveShadows || mImpl->mCastShadows)) ||
             isEmpty),
            "[entity=%u] AABB can't be empty, unless culling is disabled and "
                    "the object is not a shadow caster/receiver", entity.getId())) {
        return Error;
    }

    // we get here only if there was no POSTCONDITION errors.
    upcast(engine).createRenderable(*this, entity);
    return Success;
}

// ------------------------------------------------------------------------------------------------


namespace details {

FRenderableManager::FRenderableManager(FEngine& engine) noexcept : mEngine(engine) {
    // DON'T use engine here in the ctor, because it's not fully constructed yet.
}

FRenderableManager::~FRenderableManager() {
    // all components should have been destroyed when we get here
    // (terminate should have been called from Engine's shutdown())
    assert(mManager.getComponentCount() == 0);
}

void FRenderableManager::create(
        const RenderableManager::Builder& UTILS_RESTRICT builder, Entity entity) {
    FEngine& engine = mEngine;
    auto& manager = mManager;
    FEngine::DriverApi& driver = engine.getDriverApi();

    // If we already have an instance we can reuse parts of it without completely
    // destroying it. In particular we can reuse the UBO since it's the same for
    // all renderables
    bool canReuse = false;
    Instance ci = getInstance(entity);
    if (UTILS_UNLIKELY(ci)) {
        canReuse = true;
        destroyComponentPrimitives(engine, manager[ci].primitives);
        std::unique_ptr<Bones> const& bones = manager[ci].bones;
        if (bones && !builder->mSkinningBoneCount) {
            driver.destroyUniformBuffer(bones->handle);
        }
    }

    ci = manager.addComponent(entity);
    assert(ci);

    if (ci) {
        // create and initialize all needed RenderPrimitives
        using size_type = Slice<FRenderPrimitive>::size_type;
        Builder::Entry const * const entries = builder->mEntries;
        FRenderPrimitive* rp = new FRenderPrimitive[builder->mEntriesCount];
        for (size_t i = 0, c = builder->mEntriesCount; i < c; ++i) {
            rp[i].init(driver, entries[i]);
        }
        setPrimitives(ci, { rp, size_type(builder->mEntriesCount) });

        setAxisAlignedBoundingBox(ci, builder->mAABB);
        setLayerMask(ci, builder->mLayerMask);
        setPriority(ci, builder->mPriority);
        setCastShadows(ci, builder->mCastShadows);
        setReceiveShadows(ci, builder->mReceiveShadows);
        setCulling(ci, builder->mCulling);
        static_cast<Visibility&>(manager[ci].visibility).skinning = builder->mSkinningBoneCount > 0;

        if (!canReuse) {
            getUniformBuffer(ci) = UniformBuffer(engine.getPerRenderableUib());
            setUniformHandle(ci, driver.createUniformBuffer(getUniformBuffer(ci).getSize()));
            if (builder->mSkinningBoneCount) {
                std::unique_ptr<Bones>& bones = manager[ci].bones;

                bones.reset(new Bones); // FIXME: maybe use a pool allocator
                bones->bones = UniformBuffer(CONFIG_MAX_BONE_COUNT * sizeof(Bone));
                bones->handle = driver.createUniformBuffer(CONFIG_MAX_BONE_COUNT * sizeof(Bone));
            }
        }
        if (builder->mSkinningBoneCount) {
            std::unique_ptr<Bones> const& bones = manager[ci].bones;
            assert(bones);
            bones->count = builder->mSkinningBoneCount;
            if (builder->mBones) {
                setBones(ci, builder->mBones, builder->mSkinningBoneCount);
            } else if (builder->mBoneMatrices) {
                setBones(ci, builder->mBoneMatrices, builder->mSkinningBoneCount);
            } else {
                // initialize the bones to identity
                Bone* UTILS_RESTRICT out =
                        (Bone*)bones->bones.invalidateUniforms(0, bones->count * sizeof(Bone));
                std::fill_n(out, bones->count, Bone{});
            }
        }
    }
}

// this destroys a single component from an entity
void FRenderableManager::destroy(utils::Entity e) noexcept {
    Instance ci = getInstance(e);
    if (ci) {
        destroyComponent(ci);
        mManager.removeComponent(e);
    }
}

// this destroys all components in this manager
void FRenderableManager::terminate() noexcept {
    auto& manager = mManager;
    if (!manager.empty()) {
#ifndef NDEBUG
        slog.d << "cleaning up " << manager.getComponentCount()
               << " leaked Renderable components" << io::endl;
#endif
        while (!manager.empty()) {
            Instance ci = manager.end() - 1;
            destroyComponent(ci);
            manager.removeComponent(manager.getEntity(ci));
        }
    }
}

// This is basically a Renderable's destructor.
void FRenderableManager::destroyComponent(Instance ci) noexcept {
    auto& manager = mManager;
    FEngine& engine = mEngine;

    FEngine::DriverApi& driver = engine.getDriverApi();
    driver.destroyUniformBuffer(manager[ci].uniformsHandle);

    // See create(RenderableManager::Builder&, Entity)
    destroyComponentPrimitives(engine, manager[ci].primitives);

    // destroy the bones structures if any
    std::unique_ptr<Bones> const& bones = manager[ci].bones;
    if (bones) {
        driver.destroyUniformBuffer(bones->handle);
    }
}

void FRenderableManager::destroyComponentPrimitives(
        FEngine& engine, Slice<FRenderPrimitive>& primitives) noexcept {
    for (auto& primitive : primitives) {
        primitive.terminate(engine);
    }
    delete[] primitives.data();
}


void FRenderableManager::prepare(
        driver::DriverApi& UTILS_RESTRICT driver,
        Instance const* UTILS_RESTRICT instances,
        utils::Range<uint32_t> list) const noexcept {
    auto& manager = mManager;
    UniformBuffer           const * const UTILS_RESTRICT uniforms = manager.raw_array<UNIFORMS>();
    Handle<HwUniformBuffer> const * const UTILS_RESTRICT ubhs     = manager.raw_array<UNIFORMS_HANDLE>();
    std::unique_ptr<Bones>  const * const UTILS_RESTRICT bones    = manager.raw_array<BONES>();
    for (uint32_t index : list) {
        size_t i = instances[index].asValue();
        assert(i);  // we should never get the null instance here
        if (uniforms[i].isDirty()) {
            // update per-renderable uniform buffer
            driver.updateUniformBuffer(ubhs[i], UniformBuffer(uniforms[i]));
            uniforms[i].clean(); // clean AFTER we send to the driver
        }
        if (UTILS_UNLIKELY(bones[i])) {
            if (bones[i]->bones.isDirty()) {
                driver.updateUniformBuffer(bones[i]->handle, UniformBuffer(bones[i]->bones));
                bones[i]->bones.clean();
            }
        }
    }
}

void FRenderableManager::updateLocalUBO(Instance instance, const math::mat4f& model) noexcept {
    if (instance) {
        auto& uniforms = getUniformBuffer(instance);

        // update our uniform buffer
        uniforms.setUniform(offsetof(FEngine::PerRenderableUib, worldFromModelMatrix), model);

        // Using the inverse-transpose handles non-uniform scaling, but DOESN'T guarantee that
        // the transformed normals will have unit-length, therefore they need to be normalized
        // in the shader (that's already the case anyways, since normalization is needed after
        // interpolation).
        // Note: if the model matrix is known to be a rigid-transform, we could just use it directly.
        mat3f nm = transpose(inverse(model.upperLeft()));
        uniforms.setUniform(offsetof(FEngine::PerRenderableUib, worldFromModelNormalMatrix), nm);
    }
}

void FRenderableManager::setMaterialInstanceAt(Instance instance, uint8_t level,
        size_t primitiveIndex, FMaterialInstance const* mi) noexcept {
    if (instance) {
        Slice<FRenderPrimitive>& primitives = getRenderPrimitives(instance, level);
        if (primitiveIndex < primitives.size()) {
            primitives[primitiveIndex].setMaterialInstance(upcast(mi));
#ifndef NDEBUG
            AttributeBitset required = mi->getMaterial()->getRequiredAttributes();
            AttributeBitset declared = primitives[primitiveIndex].getEnabledAttributes();
            if (UTILS_UNLIKELY((declared & required) != required)) {
                slog.w << "[instance=" << instance.asValue() << ", primitive @ " << primitiveIndex
                       << "] missing required attributes ("
                       << required << "), declared=" << declared << io::endl;
            }
#endif
        }
    }
}

MaterialInstance* FRenderableManager::getMaterialInstanceAt(
        Instance instance, uint8_t level, size_t primitiveIndex) const noexcept {
    if (instance) {
        const Slice<FRenderPrimitive>& primitives = getRenderPrimitives(instance, level);
        if (primitiveIndex < primitives.size()) {
            // We store the material instance as const because we don't want to change it internally
            // but when the user queries it, we want to allow them to call setParameter()
            return const_cast<FMaterialInstance*>(primitives[primitiveIndex].getMaterialInstance());
        }
    }
    return nullptr;
}

void FRenderableManager::setBlendOrderAt(Instance instance, uint8_t level,
        size_t primitiveIndex, uint16_t order) noexcept {
    if (instance) {
        Slice<FRenderPrimitive>& primitives = getRenderPrimitives(instance, level);
        if (primitiveIndex < primitives.size()) {
            primitives[primitiveIndex].setBlendOrder(order);
        }
    }
}

AttributeBitset FRenderableManager::getEnabledAttributesAt(
        Instance instance, uint8_t level, size_t primitiveIndex) const noexcept {
    if (instance) {
        Slice<FRenderPrimitive> const& primitives = getRenderPrimitives(instance, level);
        if (primitiveIndex < primitives.size()) {
            return primitives[primitiveIndex].getEnabledAttributes();
        }
    }
    return AttributeBitset{};
}

void FRenderableManager::setGeometryAt(Instance instance, uint8_t level, size_t primitiveIndex,
        PrimitiveType type, FVertexBuffer* vertices, FIndexBuffer* indices,
        size_t offset, size_t count) noexcept {
    if (instance) {
        Slice<FRenderPrimitive>& primitives = getRenderPrimitives(instance, level);
        if (primitiveIndex < primitives.size()) {
            primitives[primitiveIndex].set(mEngine, type, vertices, indices, offset,
                    0, vertices->getVertexCount() - 1, count);
        }
    }
}

void FRenderableManager::setGeometryAt(Instance instance, uint8_t level, size_t primitiveIndex,
        PrimitiveType type, size_t offset, size_t count) noexcept {
    if (instance) {
        Slice<FRenderPrimitive>& primitives = getRenderPrimitives(instance, level);
        if (primitiveIndex < primitives.size()) {
            primitives[primitiveIndex].set(mEngine, type, offset, 0, 0, count);
        }
    }
}

void FRenderableManager::setBones(Instance ci,
        Bone const* UTILS_RESTRICT transforms, size_t boneCount, size_t offset) noexcept {
    if (ci) {
        std::unique_ptr<Bones> const& bones = mManager[ci].bones;
        assert(bones && offset + boneCount <= bones->count);
        if (bones) {
            boneCount = std::min(boneCount, bones->count - offset);
            Bone* UTILS_RESTRICT out = (Bone*)bones->bones.invalidateUniforms(
                    offset * sizeof(Bone),
                    boneCount * sizeof(Bone));
            std::copy_n(transforms, boneCount, out);
        }
    }
}

void FRenderableManager::setBones(Instance ci,
        math::mat4f const* UTILS_RESTRICT transforms, size_t boneCount, size_t offset) noexcept {
    if (ci) {
        std::unique_ptr<Bones> const& bones = mManager[ci].bones;
        assert(bones && offset + boneCount <= bones->count);
        if (bones) {
            boneCount = std::min(boneCount, bones->count - offset);
            Bone* UTILS_RESTRICT out = (Bone*)bones->bones.invalidateUniforms(
                    offset * sizeof(Bone),
                    boneCount * sizeof(Bone));
            for (size_t i = 0, c = bones->count; i < c; ++i) {
                mat4f const& m = transforms[i];
                out[i].unitQuaternion = m.toQuaternion();
                out[i].translation = m[3].xyz;
            }
        }
    }
}

} // namespace details


// ------------------------------------------------------------------------------------------------
// Trampoline calling into private implementation
// ------------------------------------------------------------------------------------------------

using namespace details;

bool RenderableManager::hasComponent(utils::Entity e) const noexcept {
    return upcast(this)->hasComponent(e);
}

RenderableManager::Instance
RenderableManager::getInstance(utils::Entity e) const noexcept {
    return upcast(this)->getInstance(e);
}

void RenderableManager::destroy(utils::Entity e) noexcept {
    return upcast(this)->destroy(e);
}

void RenderableManager::setAxisAlignedBoundingBox(Instance instance, const Box& aabb) noexcept {
    upcast(this)->setAxisAlignedBoundingBox(instance, aabb);
}

void RenderableManager::setLayerMask(Instance instance, uint8_t select, uint8_t values) noexcept {
    upcast(this)->setLayerMask(instance, select, values);
}

void RenderableManager::setPriority(Instance instance, uint8_t priority) noexcept {
    upcast(this)->setPriority(instance, priority);
}

void RenderableManager::setCastShadows(Instance instance, bool enable) noexcept {
    upcast(this)->setCastShadows(instance, enable);
}

void RenderableManager::setReceiveShadows(Instance instance, bool enable) noexcept {
    upcast(this)->setReceiveShadows(instance, enable);
}

bool RenderableManager::isShadowCaster(Instance instance) const noexcept {
    return upcast(this)->isShadowCaster(instance);
}

bool RenderableManager::isShadowReceiver(Instance instance) const noexcept {
    return upcast(this)->isShadowReceiver(instance);
}

const Box& RenderableManager::getAxisAlignedBoundingBox(Instance instance) const noexcept {
    return upcast(this)->getAxisAlignedBoundingBox(instance);
}

size_t RenderableManager::getPrimitiveCount(Instance instance) const noexcept {
    return upcast(this)->getPrimitiveCount(instance, 0);
}

void RenderableManager::setMaterialInstanceAt(Instance instance,
        size_t primitiveIndex, MaterialInstance const* materialInstance) noexcept {
    upcast(this)->setMaterialInstanceAt(instance, 0, primitiveIndex, upcast(materialInstance));
}

MaterialInstance* RenderableManager::getMaterialInstanceAt(
        Instance instance, size_t primitiveIndex) const noexcept {
    return upcast(this)->getMaterialInstanceAt(instance, 0, primitiveIndex);
}

void RenderableManager::setBlendOrderAt(Instance instance, size_t primitiveIndex, uint16_t order) noexcept {
    upcast(this)->setBlendOrderAt(instance, 0, primitiveIndex, order);
}

AttributeBitset RenderableManager::getEnabledAttributesAt(Instance instance, size_t primitiveIndex) const noexcept {
    return upcast(this)->getEnabledAttributesAt(instance, 0, primitiveIndex);
}

void RenderableManager::setGeometryAt(Instance instance, size_t primitiveIndex,
        PrimitiveType type, VertexBuffer* vertices, IndexBuffer* indices,
        size_t offset, size_t count) noexcept {
    upcast(this)->setGeometryAt(instance, 0, primitiveIndex,
            type, upcast(vertices), upcast(indices), offset, count);
}

void RenderableManager::setGeometryAt(RenderableManager::Instance instance, size_t primitiveIndex,
        RenderableManager::PrimitiveType type, size_t offset, size_t count) noexcept {
    upcast(this)->setGeometryAt(instance, 0, primitiveIndex, type, offset, count);
}

void RenderableManager::setBones(Instance instance,
        RenderableManager::Bone const* transforms, size_t boneCount, size_t offset) noexcept {
    upcast(this)->setBones(instance, transforms, boneCount, offset);
}

void RenderableManager::setBones(Instance instance,
        mat4f const* transforms, size_t boneCount, size_t offset) noexcept {
    upcast(this)->setBones(instance, transforms, boneCount, offset);
}

} // namespace filament
