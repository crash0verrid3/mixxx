#include "engine/effects/engineeffectchain.h"

#include "engine/effects/engineeffect.h"
#include "util/defs.h"
#include "util/sample.h"

EngineEffectChain::EngineEffectChain(const QString& id,
                                     const QSet<ChannelHandleAndGroup>& registeredInputChannels,
                                     const QSet<ChannelHandleAndGroup>& registeredOutputChannels)
        : m_id(id),
          m_enableState(EffectEnableState::Enabled),
          m_insertionType(EffectChain::INSERT),
          m_dMix(0),
          m_buffer1(MAX_BUFFER_LEN),
          m_buffer2(MAX_BUFFER_LEN) {
    // Try to prevent memory allocation.
    m_effects.reserve(256);

    for (const ChannelHandleAndGroup& inputChannel : registeredInputChannels) {
        ChannelHandleMap<ChannelStatus> outputChannelMap;
        for (const ChannelHandleAndGroup& outputChannel : registeredOutputChannels) {
            outputChannelMap.insert(outputChannel.handle(), ChannelStatus());
        }
        m_chainStatusForChannelMatrix.insert(inputChannel.handle(), outputChannelMap);
    }
}

EngineEffectChain::~EngineEffectChain() {
}

bool EngineEffectChain::addEffect(EngineEffect* pEffect, int iIndex) {
    if (iIndex < 0) {
        if (kEffectDebugOutput) {
            qDebug() << debugString()
                     << "WARNING: ADD_EFFECT_TO_CHAIN message with invalid index:"
                     << iIndex;
        }
        return false;
    }
    if (m_effects.contains(pEffect)) {
        if (kEffectDebugOutput) {
            qDebug() << debugString() << "WARNING: effect already added to EngineEffectChain:"
                     << pEffect->name();
        }
        return false;
    }

    while (iIndex >= m_effects.size()) {
        m_effects.append(NULL);
    }
    m_effects.replace(iIndex, pEffect);
    return true;
}

bool EngineEffectChain::removeEffect(EngineEffect* pEffect, int iIndex) {
    if (iIndex < 0) {
        if (kEffectDebugOutput) {
            qDebug() << debugString()
                     << "WARNING: REMOVE_EFFECT_FROM_CHAIN message with invalid index:"
                     << iIndex;
        }
        return false;
    }
    if (m_effects.at(iIndex) != pEffect) {
        qDebug() << debugString()
                 << "WARNING: REMOVE_EFFECT_FROM_CHAIN consistency error"
                 << m_effects.at(iIndex) << "loaded but received request to remove"
                 << pEffect;
        return false;
    }

    m_effects.replace(iIndex, NULL);
    return true;
}

// this is called from the engine thread onCallbackStart()
bool EngineEffectChain::updateParameters(const EffectsRequest& message) {
    // TODO(rryan): Parameter interpolation.
    m_insertionType = message.SetEffectChainParameters.insertion_type;
    m_dMix = message.SetEffectChainParameters.mix;

    if (m_enableState != EffectEnableState::Disabled && !message.SetEffectParameters.enabled) {
        m_enableState = EffectEnableState::Disabling;
    } else if (m_enableState == EffectEnableState::Disabled && message.SetEffectParameters.enabled) {
        m_enableState = EffectEnableState::Enabling;
    }
    return true;
}

bool EngineEffectChain::processEffectsRequest(EffectsRequest& message,
                                              EffectsResponsePipe* pResponsePipe) {
    EffectsResponse response(message);
    switch (message.type) {
        case EffectsRequest::ADD_EFFECT_TO_CHAIN:
            if (kEffectDebugOutput) {
                qDebug() << debugString() << this << "ADD_EFFECT_TO_CHAIN"
                         << message.AddEffectToChain.pEffect
                         << message.AddEffectToChain.iIndex;
            }
            response.success = addEffect(message.AddEffectToChain.pEffect,
                                         message.AddEffectToChain.iIndex);
            break;
        case EffectsRequest::REMOVE_EFFECT_FROM_CHAIN:
            if (kEffectDebugOutput) {
                qDebug() << debugString() << this << "REMOVE_EFFECT_FROM_CHAIN"
                         << message.RemoveEffectFromChain.pEffect
                         << message.RemoveEffectFromChain.iIndex;
            }
            response.success = removeEffect(message.RemoveEffectFromChain.pEffect,
                                            message.RemoveEffectFromChain.iIndex);
            break;
        case EffectsRequest::SET_EFFECT_CHAIN_PARAMETERS:
            if (kEffectDebugOutput) {
                qDebug() << debugString() << this << "SET_EFFECT_CHAIN_PARAMETERS"
                         << "enabled" << message.SetEffectChainParameters.enabled
                         << "mix" << message.SetEffectChainParameters.mix;
            }
            response.success = updateParameters(message);
            break;
        case EffectsRequest::ENABLE_EFFECT_CHAIN_FOR_INPUT_CHANNEL:
            if (kEffectDebugOutput) {
                qDebug() << debugString() << this
                         << "ENABLE_EFFECT_CHAIN_FOR_INPUT_CHANNEL"
                         << message.pTargetChain
                         << *message.EnableInputChannelForChain.pChannelHandle;
            }
            response.success = enableForInputChannel(
                  message.EnableInputChannelForChain.pChannelHandle,
                  message.EnableInputChannelForChain.pEffectStatesMapArray);
            break;
        case EffectsRequest::DISABLE_EFFECT_CHAIN_FOR_INPUT_CHANNEL:
            if (kEffectDebugOutput) {
                qDebug() << debugString() << this
                         << "DISABLE_EFFECT_CHAIN_FOR_INPUT_CHANNEL"
                         << message.pTargetChain
                         << *message.DisableInputChannelForChain.pChannelHandle;
            }
            response.success = disableForInputChannel(
                    message.DisableInputChannelForChain.pChannelHandle);
            break;
        default:
            return false;
    }
    pResponsePipe->writeMessages(&response, 1);
    return true;
}

bool EngineEffectChain::enableForInputChannel(const ChannelHandle* inputHandle,
        EffectStatesMapArray* statesForEffectsInChain) {
    if (kEffectDebugOutput) {
        qDebug() << "EngineEffectChain::enableForInputChannel" << this << inputHandle;
    }
    auto& outputMap = m_chainStatusForChannelMatrix[*inputHandle];
    for (auto&& outputChannelStatus : outputMap) {
        VERIFY_OR_DEBUG_ASSERT(outputChannelStatus.enable_state !=
                EffectEnableState::Enabled) {
            for (auto&& pStatesMap : *statesForEffectsInChain) {
                for (auto&& pState : pStatesMap) {
                    delete pState;
                }
            }
            return false;
        }
        outputChannelStatus.enable_state = EffectEnableState::Enabling;
    }
    for (int i = 0; i < m_effects.size(); ++i) {
        if (m_effects[i] != nullptr) {
            if (kEffectDebugOutput) {
                qDebug() << "EngineEffectChain::enableForInputChannel" << this
                         << "loading states for effect" << i;
            }
            EffectStatesMap* pStatesMap = &(*statesForEffectsInChain)[i];
            VERIFY_OR_DEBUG_ASSERT(pStatesMap) {
                return false;
            }
            m_effects[i]->loadStatesForInputChannel(inputHandle, pStatesMap);
        }
    }
    return true;
}

bool EngineEffectChain::disableForInputChannel(const ChannelHandle* inputHandle) {
    auto& outputMap = m_chainStatusForChannelMatrix[*inputHandle];
    for (auto&& outputChannelStatus : outputMap) {
        if (outputChannelStatus.enable_state != EffectEnableState::Disabled) {
            outputChannelStatus.enable_state = EffectEnableState::Disabling;
        }
    }
    // Do not call deleteStatesForInputChannel here because the EngineEffects'
    // process() method needs to run one last time before deleting the states.
    // deleteStatesForInputChannel needs to be called from the main thread after
    // the succesful EffectsResponse is returned by the MessagePipe FIFO.
    return true;
}

// Called from the main thread for garbage collection after an input channel is disabled
void EngineEffectChain::deleteStatesForInputChannel(const ChannelHandle* inputChannel) {
    for (EngineEffect* pEffect : m_effects) {
        if (pEffect != nullptr) {
            pEffect->deleteStatesForInputChannel(inputChannel);
        }
    }
}

EngineEffectChain::ChannelStatus& EngineEffectChain::getChannelStatus(
        const ChannelHandle& inputHandle,
        const ChannelHandle& outputHandle) {
    ChannelStatus& status = m_chainStatusForChannelMatrix[inputHandle][outputHandle];
    return status;
}

bool EngineEffectChain::process(const ChannelHandle& inputHandle,
                                const ChannelHandle& outputHandle,
                                CSAMPLE* pIn, CSAMPLE* pOut,
                                const unsigned int numSamples,
                                const unsigned int sampleRate,
                                const GroupFeatureState& groupFeatures) {
    // Compute the effective enable state from the channel input routing switch and
    // the chain's enable state. When either of these are turned on/off, send the
    // effects the intermediate enabling/disabling signal.
    // If the EngineEffect is not disabled for the channel, it will pass the
    // intermediate state down to the EffectProcessor, which is then responsible for reacting
    // appropriately, for example the Echo effect clears its internal buffer for the channel
    // when it gets the intermediate disabling signal.

    ChannelStatus& channelStatus = m_chainStatusForChannelMatrix[inputHandle][outputHandle];
    EffectEnableState effectiveChainEnableState = channelStatus.enable_state;

    if (m_enableState != EffectEnableState::Enabled) {
        effectiveChainEnableState = m_enableState;
    }

    CSAMPLE currentWetGain = m_dMix;
    CSAMPLE lastCallbackWetGain = channelStatus.old_gain;

    bool processingOccured = false;
    if (effectiveChainEnableState != EffectEnableState::Disabled) {
        // Ramping code inside the effects need to access the original samples
        // after writing to the output buffer. This requires not to use the same buffer
        // for in and output: Also, ChannelMixer::applyEffectsAndMixChannels
        // requires that the input buffer does not get modified.
        bool processingOccured = false;
        CSAMPLE* pIntermediateInput = pIn;
        CSAMPLE* pIntermediateOutput;

        for (EngineEffect* pEffect: m_effects) {
            if (pEffect != nullptr) {
                // Select an unused intermediate buffer for the next output
                if (pIntermediateInput == m_buffer1.data()) {
                    pIntermediateOutput = m_buffer2.data();
                } else {
                    pIntermediateOutput = m_buffer1.data();
                }

                if (pEffect->process(inputHandle, outputHandle,
                                     pIntermediateInput, pIntermediateOutput,
                                     numSamples, sampleRate,
                                     effectiveChainEnableState, groupFeatures)) {
                    processingOccured = true;
                    // Output of this effect becomes the input of the next effect
                    pIntermediateInput = pIntermediateOutput;
                }
            }
        }

        if (processingOccured) {
            // pIntermediateInput is the output of the last processed effect. It would be the
            // intermediate input of the next effect if there was one.
            if (m_insertionType == EffectChain::INSERT) {
                // INSERT mode: output = input * (1-wet) + effect(input) * wet
                SampleUtil::copy2WithRampingGain(
                        pOut,
                        pIn, 1.0 - lastCallbackWetGain, 1.0 - currentWetGain,
                        pIntermediateInput, lastCallbackWetGain, currentWetGain,
                        numSamples);
            } else {
                // SEND mode: output = input + effect(input) * wet
                SampleUtil::copy2WithRampingGain(
                        pOut,
                        pIn, 1.0, 1.0,
                        pIntermediateInput, lastCallbackWetGain, currentWetGain,
                        numSamples);
            }
        }
    }

    channelStatus.old_gain = currentWetGain;

    // If the EffectProcessors have been sent a signal for the intermediate
    // enabling/disabling state, set the channel state or chain state
    // to the fully enabled/disabled state for the next engine callback.

    EffectEnableState& chainOnChannelEnableState = channelStatus.enable_state;
    if (chainOnChannelEnableState == EffectEnableState::Disabling) {
        chainOnChannelEnableState = EffectEnableState::Disabled;
    } else if (chainOnChannelEnableState == EffectEnableState::Enabling) {
        chainOnChannelEnableState = EffectEnableState::Enabled;
    }

    if (m_enableState == EffectEnableState::Disabling) {
        m_enableState = EffectEnableState::Disabled;
    } else if (m_enableState == EffectEnableState::Enabling) {
        m_enableState = EffectEnableState::Enabled;
    }

    return processingOccured;
}
