/*!
 *  Copyright (c) 2023 by Contributors
 * \file serve/engine_actions/batch_draft.cc
 */

#include <numeric>

#include "../config.h"
#include "../model.h"
#include "../sampler/sampler.h"
#include "action.h"
#include "action_commons.h"

namespace mlc {
namespace llm {
namespace serve {

/*!
 * \brief The action that runs draft proposal for requests in the
 * `running_queue` of engine state. Preempt low-priority requests
 * accordingly when it is impossible to decode all the running requests.
 */
class BatchDraftActionObj : public EngineActionObj {
 public:
  explicit BatchDraftActionObj(Array<Model> models, LogitProcessor logit_processor, Sampler sampler,
                               std::vector<ModelWorkspace> model_workspaces,
                               DraftTokenWorkspaceManager draft_token_workspace_manager,
                               EngineConfig engine_config,
                               Optional<EventTraceRecorder> trace_recorder, int draft_length)
      : models_(std::move(models)),
        logit_processor_(std::move(logit_processor)),
        sampler_(std::move(sampler)),
        model_workspaces_(std::move(model_workspaces)),
        draft_token_workspace_manager_(std::move(draft_token_workspace_manager)),
        engine_config_(std::move(engine_config)),
        trace_recorder_(std::move(trace_recorder)),
        draft_length_(draft_length) {
    ICHECK_GT(draft_length_, 0);
  }

  Array<Request> Step(EngineState estate) final {
    // - Only run spec decode when there are two models (llm+ssm) and >=1 running requests.
    if (models_.size() != 2 || estate->running_queue.empty()) {
      return {};
    }

    // Preempt request state entries when decode cannot apply.
    std::vector<RequestStateEntry> running_rsentries = estate->GetRunningRequestStateEntries();
    while (!CanDecode(running_rsentries.size())) {
      if (estate->prefix_cache->TryFreeMemory()) continue;
      RequestStateEntry preempted = PreemptLastRunningRequestStateEntry(
          estate, models_, draft_token_workspace_manager_, trace_recorder_);
      if (preempted.same_as(running_rsentries.back())) {
        running_rsentries.pop_back();
      }
    }

    auto tstart = std::chrono::high_resolution_clock::now();

    int num_rsentries = running_rsentries.size();
    ICHECK_GT(num_rsentries, 0)
        << "There should be at least one request state entry that can run decode. "
           "Possible failure reason: none of the prefill phase of the running requests is finished";
    ICHECK_LE(num_rsentries, engine_config_->max_num_sequence)
        << "The number of running requests exceeds the max number of sequence in EngineConfig. "
           "Possible failure reason: the prefill action allows new sequence in regardless of the "
           "max num sequence.";
    Array<String> request_ids;
    std::vector<int64_t> request_internal_ids;
    Array<GenerationConfig> generation_cfg;
    std::vector<RandomGenerator*> rngs;
    std::vector<std::vector<int>> draft_token_indices;
    request_ids.reserve(num_rsentries);
    request_internal_ids.reserve(num_rsentries);
    generation_cfg.reserve(num_rsentries);
    draft_token_indices.reserve(num_rsentries);
    for (const RequestStateEntry& rsentry : running_rsentries) {
      request_ids.push_back(rsentry->request->id);
      request_internal_ids.push_back(rsentry->mstates[0]->internal_id);
      generation_cfg.push_back(rsentry->request->generation_cfg);
      rngs.push_back(&rsentry->rng);
    }

    // The first model doesn't get involved in draft proposal.
    for (int model_id = 1; model_id < static_cast<int>(models_.size()); ++model_id) {
      // Collect
      // - the last committed token,
      // - the request model state of each request,
      // - the number of tokens for each request to send into the model (it may
      // be more than one if the draft model is lagging behind the main model, when
      // the engine switches from normal batch decode mode to speculative decoding mode).
      std::vector<int> input_tokens;
      Array<RequestModelState> mstates;
      std::vector<int> lengths;
      input_tokens.reserve(num_rsentries);
      mstates.reserve(num_rsentries);
      lengths.reserve(num_rsentries);
      for (const RequestStateEntry& rsentry : running_rsentries) {
        mstates.push_back(rsentry->mstates[model_id]);
      }
      // draft_length_ rounds of draft proposal.
      for (int draft_id = 0; draft_id < draft_length_; ++draft_id) {
        auto tdraft_start = std::chrono::high_resolution_clock::now();
        // prepare new input tokens
        input_tokens.clear();
        draft_token_indices.clear();
        lengths.clear();
        for (int i = 0; i < num_rsentries; ++i) {
          // The first draft proposal uses the last committed token.
          if (draft_id == 0) {
            CHECK_LE(mstates[i]->committed_tokens.size(),
                     running_rsentries[i]->mstates[0]->committed_tokens.size());
            ICHECK_EQ(mstates[i]->num_tokens_for_next_decode, 1);
            input_tokens.push_back(mstates[i]->committed_tokens.back().GetTokenId());
            lengths.push_back(running_rsentries[i]->mstates[0]->committed_tokens.size() -
                              mstates[i]->committed_tokens.size() + 1);
            for (size_t j = mstates[i]->committed_tokens.size();
                 j < running_rsentries[i]->mstates[0]->committed_tokens.size(); ++j) {
              // This draft model is lagging behind the main model.
              // It may happen when the engine just switches from the normal batch decode
              // mode to the speculative decoding mode.
              // In this case, we need to prefill the misaligned tokens into the draft model.
              mstates[i]->CommitToken(running_rsentries[i]->mstates[0]->committed_tokens[j]);
              input_tokens.push_back(
                  running_rsentries[i]->mstates[0]->committed_tokens[j].GetTokenId());
            }
            mstates[i]->num_tokens_for_next_decode = 0;
            draft_token_indices.emplace_back(std::vector<int>{-1});
          } else {
            CHECK_EQ(mstates[i]->committed_tokens.size(),
                     running_rsentries[i]->mstates[0]->committed_tokens.size());
            ICHECK(!mstates[i]->draft_output_tokens.empty());
            input_tokens.push_back(mstates[i]->draft_output_tokens.back().GetTokenId());
            lengths.push_back(1);
            draft_token_indices.emplace_back(
                std::vector<int>{static_cast<int>(mstates[i]->draft_output_tokens.size()) - 1});
          }
        }

        // - Compute embeddings.
        RECORD_EVENT(trace_recorder_, request_ids, "start proposal embedding");
        ObjectRef embeddings =
            models_[model_id]->TokenEmbed({IntTuple{input_tokens.begin(), input_tokens.end()}});
        RECORD_EVENT(trace_recorder_, request_ids, "finish proposal embedding");

        // - Invoke model decode.
        RECORD_EVENT(trace_recorder_, request_ids, "start proposal decode");
        NDArray logits{nullptr};
        if (input_tokens.size() == num_rsentries) {
          // Each request entry only has one token to feed into the draft model.
          logits = models_[model_id]->BatchDecode(embeddings, request_internal_ids);
          ICHECK_EQ(logits->ndim, 3);
          ICHECK_EQ(logits->shape[0], num_rsentries);
          ICHECK_EQ(logits->shape[1], 1);
        } else {
          // There exists some request entry which has more than one token to feed.
          // It may happen when the engine just switches from the normal batch decode
          // mode to the speculative decoding mode.
          logits = models_[model_id]->BatchPrefill(embeddings, request_internal_ids, lengths);
          ICHECK_EQ(logits->ndim, 3);
          ICHECK_EQ(logits->shape[0], 1);
          ICHECK_EQ(logits->shape[1], num_rsentries);
        }
        CHECK_EQ(lengths.size(), num_rsentries);
        RECORD_EVENT(trace_recorder_, request_ids, "finish proposal decode");

        // - Update logits.
        logits = logits.CreateView({num_rsentries, logits->shape[2]}, logits->dtype);
        logit_processor_->InplaceUpdateLogits(logits, generation_cfg, mstates, request_ids, nullptr,
                                              &mstates, &draft_token_indices);

        // - Compute probability distributions.
        NDArray probs_on_device =
            logit_processor_->ComputeProbsFromLogits(logits, generation_cfg, request_ids);

        // - Commit the prefix cache changes from previous round of action.
        // Note: we commit prefix cache changes here to overlap this commit with the GPU execution.
        estate->prefix_cache->CommitSequenceExtention();

        // - Sample tokens.
        // Fill range [0, num_rsentries) into `sample_indices`.
        std::vector<int> sample_indices(num_rsentries);
        std::iota(sample_indices.begin(), sample_indices.end(), 0);
        std::vector<NDArray> prob_dist;
        NDArray renormalized_probs = sampler_->BatchRenormalizeProbsByTopP(
            probs_on_device, sample_indices, request_ids, generation_cfg);
        std::vector<SampleResult> sample_results = sampler_->BatchSampleTokensWithProbAfterTopP(
            renormalized_probs, sample_indices, request_ids, generation_cfg, rngs);
        ICHECK_EQ(sample_results.size(), num_rsentries);

        // - Add draft token to the state.
        draft_token_workspace_manager_->AllocSlots(num_rsentries, &draft_token_slots_);
        models_[model_id]->ScatterDraftProbs(probs_on_device, draft_token_slots_,
                                             &model_workspaces_[0].draft_probs_storage);
        for (int i = 0; i < num_rsentries; ++i) {
          int64_t parent_idx = static_cast<int64_t>(mstates[i]->draft_output_tokens.size()) - 1;
          mstates[i]->AddDraftToken(sample_results[i], draft_token_slots_[i], parent_idx);
        }

        auto tdraft_end = std::chrono::high_resolution_clock::now();
        estate->metrics.UpdateDraftTimeByBatchSize(
            num_rsentries, static_cast<double>((tdraft_end - tdraft_start).count()) / 1e9);
      }
    }

    auto tend = std::chrono::high_resolution_clock::now();
    estate->metrics.engine_decode_time_sum += static_cast<double>((tend - tstart).count()) / 1e9;

    return {};
  }

 private:
  /*! \brief Check if the input requests can be decoded under conditions. */
  bool CanDecode(int num_rsentries) {
    // The first model is not involved in draft proposal.
    for (int model_id = 1; model_id < static_cast<int>(models_.size()); ++model_id) {
      // Check if the model has enough available pages.
      int num_available_pages = models_[model_id]->GetNumAvailablePages();
      if (num_rsentries > num_available_pages) {
        return false;
      }
    }
    return true;
  }

  /*! \brief The model to run draft generation in speculative decoding. */
  Array<Model> models_;
  /*! \brief The logit processor. */
  LogitProcessor logit_processor_;
  /*! \brief The sampler to sample new tokens. */
  Sampler sampler_;
  /*! \brief The model workspaces. */
  std::vector<ModelWorkspace> model_workspaces_;
  /*! \brief The draft token workspace manager. */
  DraftTokenWorkspaceManager draft_token_workspace_manager_;
  /*! \brief The engine config. */
  EngineConfig engine_config_;
  /*! \brief Event trace recorder. */
  Optional<EventTraceRecorder> trace_recorder_;
  /*! \brief Draft proposal length */
  int draft_length_;
  /*! \brief Temporary buffer to store the slots of the current draft tokens */
  std::vector<int> draft_token_slots_;
};

EngineAction EngineAction::BatchDraft(Array<Model> models, LogitProcessor logit_processor,
                                      Sampler sampler, std::vector<ModelWorkspace> model_workspaces,
                                      DraftTokenWorkspaceManager draft_token_workspace_manager,
                                      EngineConfig engine_config,
                                      Optional<EventTraceRecorder> trace_recorder,
                                      int draft_length) {
  return EngineAction(make_object<BatchDraftActionObj>(
      std::move(models), std::move(logit_processor), std::move(sampler),
      std::move(model_workspaces), std::move(draft_token_workspace_manager),
      std::move(engine_config), std::move(trace_recorder), draft_length));
}

}  // namespace serve
}  // namespace llm
}  // namespace mlc
