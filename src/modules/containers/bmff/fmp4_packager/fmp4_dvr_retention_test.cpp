//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <vector>

#include "duration_boundary_policy.h"
#include "fmp4_test_fixtures.h"

// DVR retention: a segment beyond the maximum DVR duration leaves the playlist
// first, and its file stays servable for a few more segments

namespace
{
	constexpr uint64_t kSegmentDurationMs = 1000;
	constexpr double kChunkDurationMs = 500.0;
	constexpr int kGopFrames = 30;	// keyframes at multiples of 1000 ms at 30 fps
	constexpr uint32_t kTrackId = 0;
	constexpr const char *kStreamTag = "dvr_test";

	class DeletionRecorder : public fmp4_test::NullStorageObserver
	{
	public:
		void OnMediaSegmentDeleted(const int32_t &track_id, const uint32_t &segment_number) override
		{
			deleted_segment_numbers.push_back(segment_number);
		}

		std::vector<uint32_t> deleted_segment_numbers;
	};

	// Gives each run its own DVR directory and removes it even when an assertion fails
	class FMP4DvrRetentionTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			std::filesystem::remove_all(_dvr_path);
		}

		void TearDown() override
		{
			std::filesystem::remove_all(_dvr_path);
		}

		std::filesystem::path SegmentFilePath(uint32_t segment_number) const
		{
			return _dvr_path / kStreamTag / std::to_string(kTrackId) / (std::to_string(segment_number) + ".m4s");
		}

		const std::filesystem::path _dvr_path = std::filesystem::temp_directory_path() / ("ome_fmp4_dvr_test_" + std::to_string(::getpid()));
	};
}  // namespace

TEST_F(FMP4DvrRetentionTest, SegmentLeavingThePlaylistStaysServableForTheRetention)
{
	auto track = fmp4_test::MakeVideoTrack(kTrackId, 30.0, kGopFrames);

	bmff::SegmentBoundaryPolicy::Config policy_config;
	policy_config.segment_duration_ms = kSegmentDurationMs;
	policy_config.chunk_duration_ms = kChunkDurationMs;
	auto policy = std::make_shared<bmff::DurationBoundaryPolicy>(policy_config, false);

	bmff::FMP4Storage::Config storage_config;
	storage_config.max_segments = 3;
	storage_config.segment_duration_ms = kSegmentDurationMs;
	storage_config.dvr_enabled = true;
	storage_config.dvr_storage_path = _dvr_path.string().c_str();
	storage_config.dvr_duration_sec = 3;

	bmff::FMP4Packager::Config packager_config;
	packager_config.chunk_duration_ms = kChunkDurationMs;

	auto recorder = std::make_shared<DeletionRecorder>();
	auto storage = std::make_shared<bmff::FMP4Storage>(recorder, track, storage_config, kStreamTag, policy);
	auto packager = std::make_shared<bmff::FMP4Packager>(storage, policy, track, nullptr, packager_config);

	// 20 seconds of 30 fps video
	for (int64_t index = 0; index < 600; index++)
	{
		const int64_t dts = index * 100 / 3;
		const int64_t duration = (index + 1) * 100 / 3 - dts;
		ASSERT_TRUE(packager->AppendSample(fmp4_test::MakeVideoFrame(kTrackId, dts, duration, (index % kGopFrames) == 0)));
	}

	const auto &deleted = recorder->deleted_segment_numbers;
	const size_t retention = bmff::FMP4Storage::kSegmentRetentionCount;
	ASSERT_GT(deleted.size(), retention);

	// The latest segments that left the playlist are still served from their files
	for (size_t i = deleted.size() - retention; i < deleted.size(); i++)
	{
		auto segment = storage->GetSegment(deleted[i]);
		ASSERT_NE(segment, nullptr) << "segment " << deleted[i] << " left the playlist and is no longer served";
		EXPECT_GT(segment->GetDataLength(), 0u);
		EXPECT_TRUE(std::filesystem::exists(SegmentFilePath(deleted[i])));
	}

	// The one before them is gone, record and file
	auto expired_segment_number = deleted[deleted.size() - retention - 1];
	EXPECT_EQ(storage->GetSegment(expired_segment_number), nullptr);
	EXPECT_FALSE(std::filesystem::exists(SegmentFilePath(expired_segment_number)));
}
