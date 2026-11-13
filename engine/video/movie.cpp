#include "engine/video/movie.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <mkvparser/mkvparser.h>
#include <ogg/ogg.h>

#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

constexpr std::string_view kAv1Codec = "V_AV1";
constexpr std::string_view kOpusCodec = "A_OPUS";
constexpr double kNanosecondsPerSecond = 1e9;
// The frame length of a movie with one video packet.
constexpr double kSingleFrameSeconds = 1.0 / 30.0;

// Opus stream headers (RFC 7845): the ID header is the track's CodecPrivate.
constexpr std::string_view kOpusHeadMagic = "OpusHead";
constexpr std::size_t kOpusHeadMinBytes = 19;
constexpr std::string_view kOpusTagsMagic = "OpusTags";
constexpr int kOggSerial = 1;

// How many 48 kHz samples one frame of an Opus packet holds, by the configuration number in its
// table of contents byte (RFC 6716 section 3.1): SILK 10 to 60 ms, hybrid 10 or 20 ms, CELT 2.5
// to 20 ms.
constexpr std::array<std::uint64_t, 4> kSilkFrameSamples{480, 960, 1920, 2880};
constexpr std::array<std::uint64_t, 2> kHybridFrameSamples{480, 960};
constexpr std::array<std::uint64_t, 4> kCeltFrameSamples{120, 240, 480, 960};
constexpr unsigned kOpusConfigShift = 3;
constexpr unsigned kOpusFirstHybridConfig = 12;
constexpr unsigned kOpusFirstCeltConfig = 16;
constexpr unsigned kOpusFramesCodeMask = 3;
constexpr unsigned kOpusTwoFrames = 2;
constexpr unsigned kOpusArbitraryFrames = 3;
constexpr unsigned kOpusFrameCountMask = 0x3F;

std::uint64_t opus_packet_samples(std::span<const std::byte> packet) {
  if (packet.empty()) {
    return 0;
  }
  const auto toc = std::to_integer<unsigned>(packet.front());
  const unsigned config = toc >> kOpusConfigShift;
  std::uint64_t per_frame = 0;
  if (config < kOpusFirstHybridConfig) {
    per_frame = kSilkFrameSamples.at(config % kSilkFrameSamples.size());
  } else if (config < kOpusFirstCeltConfig) {
    per_frame = kHybridFrameSamples.at(config % kHybridFrameSamples.size());
  } else {
    per_frame = kCeltFrameSamples.at(config % kCeltFrameSamples.size());
  }
  const unsigned code = toc & kOpusFramesCodeMask;
  if (code == 0) {
    return per_frame;
  }
  if (code < kOpusArbitraryFrames) {
    return per_frame * kOpusTwoFrames;
  }
  return packet.size() > 1
             ? per_frame * (std::to_integer<unsigned>(packet.subspan(1).front()) & kOpusFrameCountMask)
             : 0;
}

// Reads from bytes in memory.
class MemoryReader final : public mkvparser::IMkvReader {
 public:
  explicit MemoryReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  [[nodiscard]] std::size_t size() const { return bytes_.size(); }

  int Read(long long pos, long len, unsigned char* buf) override {
    if (pos < 0 || len < 0 ||
        static_cast<unsigned long long>(pos) + static_cast<unsigned long long>(len) >
            bytes_.size()) {
      return -1;
    }
    const auto count = static_cast<std::size_t>(len);
    std::ranges::transform(bytes_.subspan(static_cast<std::size_t>(pos), count),
                           std::span{buf, count}.begin(),
                           [](std::byte b) { return std::to_integer<unsigned char>(b); });
    return 0;
  }

  int Length(long long* total, long long* available) override {
    *total = static_cast<long long>(bytes_.size());
    *available = *total;
    return 0;
  }

 private:
  std::span<const std::byte> bytes_;
};

struct TrackInfo {
  long number = 0;
  std::string codec{};
  std::vector<std::byte> codec_private{};
  Size size{};
};

struct ParsedWebm {
  std::optional<TrackInfo> video{};
  std::optional<TrackInfo> audio{};
  std::vector<MovieData::Packet> video_packets{};
  std::vector<MovieData::Packet> audio_packets{};
};

TrackInfo describe(const mkvparser::Track& track) {
  TrackInfo info{.number = track.GetNumber()};
  if (const char* codec = track.GetCodecId()) {
    info.codec = codec;
  }
  std::size_t private_size = 0;
  if (const unsigned char* data = track.GetCodecPrivate(private_size)) {
    info.codec_private.reserve(private_size);
    std::ranges::transform(std::span{data, private_size}, std::back_inserter(info.codec_private),
                           [](unsigned char b) { return std::byte{b}; });
  }
  if (const auto* video = dynamic_cast<const mkvparser::VideoTrack*>(&track)) {
    const long long width = video->GetWidth();
    const long long height = video->GetHeight();
    if (width < 0 || height < 0 || width > kMaxMovieDimension || height > kMaxMovieDimension) {
      throw MovieError{"has a picture that is too large"};
    }
    info.size = Size{.width = static_cast<int>(width), .height = static_cast<int>(height)};
  }
  return info;
}

// The packet of a block's one frame.
MovieData::Packet read_packet(MemoryReader& reader, const mkvparser::Block& block,
                              const mkvparser::Cluster& cluster) {
  if (block.GetFrameCount() != 1) {
    throw MovieError{"has laced blocks, which the engine does not read"};
  }
  const mkvparser::Block::Frame& frame = block.GetFrame(0);
  if (frame.len < 0 || frame.pos < 0 ||
      static_cast<std::size_t>(frame.pos) + static_cast<std::size_t>(frame.len) > reader.size()) {
    throw MovieError{"is not a WebM file"};
  }
  std::vector<unsigned char> raw(static_cast<std::size_t>(frame.len));
  if (frame.Read(&reader, raw.data()) != 0) {
    throw MovieError{"is not a WebM file"};
  }
  MovieData::Packet packet;
  packet.data.reserve(raw.size());
  std::ranges::transform(raw, std::back_inserter(packet.data),
                         [](unsigned char b) { return std::byte{b}; });
  packet.time = std::max(0.0, static_cast<double>(block.GetTime(&cluster)) / kNanosecondsPerSecond);
  return packet;
}

ParsedWebm parse_webm(std::span<const std::byte> webm) {
  MemoryReader reader{webm};
  long long position = 0;
  mkvparser::EBMLHeader header;
  if (header.Parse(&reader, position) < 0) {
    throw MovieError{"is not a WebM file"};
  }
  mkvparser::Segment* opened = nullptr;
  if (mkvparser::Segment::CreateInstance(&reader, position, opened) != 0 || opened == nullptr) {
    throw MovieError{"is not a WebM file"};
  }
  const std::unique_ptr<mkvparser::Segment> segment{opened};
  if (segment->Load() < 0) {
    throw MovieError{"is not a complete WebM file"};
  }
  ParsedWebm parsed;
  const mkvparser::Tracks* tracks = segment->GetTracks();
  if (tracks == nullptr) {
    throw MovieError{"is not a WebM file with a video track"};
  }
  for (unsigned long i = 0; i < tracks->GetTracksCount(); ++i) {
    const mkvparser::Track* track = tracks->GetTrackByIndex(i);
    if (track == nullptr) {
      continue;
    }
    if (track->GetType() == mkvparser::Track::kVideo && !parsed.video) {
      parsed.video = describe(*track);
    } else if (track->GetType() == mkvparser::Track::kAudio && !parsed.audio) {
      parsed.audio = describe(*track);
    }
  }
  for (const mkvparser::Cluster* cluster = segment->GetFirst();
       cluster != nullptr && !cluster->EOS(); cluster = segment->GetNext(cluster)) {
    const mkvparser::BlockEntry* entry = nullptr;
    long status = cluster->GetFirst(entry);
    while (status == 0 && entry != nullptr && !entry->EOS()) {
      const mkvparser::Block* block = entry->GetBlock();
      if (block != nullptr) {
        const auto number = static_cast<long>(block->GetTrackNumber());
        std::vector<MovieData::Packet>* target = nullptr;
        if (parsed.video && number == parsed.video->number) {
          target = &parsed.video_packets;
        } else if (parsed.audio && number == parsed.audio->number) {
          target = &parsed.audio_packets;
        }
        if (target != nullptr) {
          if (target->size() >= kMaxMoviePackets) {
            throw MovieError{"has too many packets"};
          }
          target->push_back(read_packet(reader, *block, *cluster));
        }
      }
      const mkvparser::BlockEntry* next = nullptr;
      status = cluster->GetNext(entry, next);
      entry = next;
    }
    if (status < 0) {
      throw MovieError{"is not a complete WebM file"};
    }
  }
  return parsed;
}

// Writes Opus packets as an Ogg stream (RFC 7845), which opusfile reads.
class OggWriter {
 public:
  OggWriter() { ogg_stream_init(&stream_, kOggSerial); }
  OggWriter(const OggWriter&) = delete;
  OggWriter(OggWriter&&) = delete;
  OggWriter& operator=(const OggWriter&) = delete;
  OggWriter& operator=(OggWriter&&) = delete;
  ~OggWriter() { ogg_stream_clear(&stream_); }

  // `granule` is the 48 kHz sample count at the end of the packet. A header packet is written
  // into a page of its own.
  void packet(std::span<const std::byte> data, std::int64_t granule, bool last, bool header) {
    std::vector<unsigned char> copy(data.size());
    std::ranges::transform(data, copy.begin(),
                           [](std::byte b) { return std::to_integer<unsigned char>(b); });
    ogg_packet packet{};
    packet.packet = copy.data();
    packet.bytes = static_cast<long>(copy.size());
    packet.b_o_s = count_ == 0 ? 1 : 0;
    packet.e_o_s = last ? 1 : 0;
    packet.granulepos = granule;
    packet.packetno = count_++;
    if (ogg_stream_packetin(&stream_, &packet) != 0) {
      throw MovieError{"has audio that cannot be read"};
    }
    ogg_page page{};
    if (header || last) {
      while (ogg_stream_flush(&stream_, &page) != 0) {
        append(page);
      }
    }
    while (ogg_stream_pageout(&stream_, &page) != 0) {
      append(page);
    }
  }

  [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

 private:
  void append(const ogg_page& page) {
    const auto header =
        std::as_bytes(std::span{page.header, static_cast<std::size_t>(page.header_len)});
    const auto body = std::as_bytes(std::span{page.body, static_cast<std::size_t>(page.body_len)});
    bytes_.insert(bytes_.end(), header.begin(), header.end());
    bytes_.insert(bytes_.end(), body.begin(), body.end());
  }

  ogg_stream_state stream_{};
  ogg_int64_t count_ = 0;
  std::vector<std::byte> bytes_;
};

std::vector<std::byte> text_bytes(std::string_view text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

bool starts_with(std::span<const std::byte> bytes, std::string_view prefix) {
  return bytes.size() >= prefix.size() &&
         std::ranges::equal(bytes.first(prefix.size()), text_bytes(prefix));
}

// The audio track as an Ogg Opus file. `head` is the track's CodecPrivate, the Opus ID header.
std::vector<std::byte> opus_to_ogg(const TrackInfo& track,
                                   const std::vector<MovieData::Packet>& packets) {
  if (track.codec != kOpusCodec) {
    throw MovieError{"has audio that is not Opus"};
  }
  if (track.codec_private.size() < kOpusHeadMinBytes ||
      !starts_with(track.codec_private, kOpusHeadMagic)) {
    throw MovieError{"has an Opus track without a valid header"};
  }
  OggWriter ogg;
  ogg.packet(track.codec_private, 0, false, true);
  // The comment header: the magic, an empty vendor string and no comments.
  std::vector<std::byte> tags = text_bytes(kOpusTagsMagic);
  tags.resize(tags.size() + 2 * sizeof(std::uint32_t), std::byte{0});
  ogg.packet(tags, 0, false, true);
  std::int64_t granule = 0;
  for (std::size_t i = 0; i < packets.size(); ++i) {
    const std::vector<std::byte>& data = packets.at(i).data;
    granule += static_cast<std::int64_t>(opus_packet_samples(data));
    ogg.packet(data, granule, i + 1 == packets.size(), false);
  }
  return ogg.take();
}

std::string quoted_movie(std::string_view path) {
  std::string text = "movie '";
  text += path;
  text += "'";
  return text;
}

}  // namespace

MovieData open_movie(std::span<const std::byte> webm) {
  ParsedWebm parsed = parse_webm(webm);
  if (!parsed.video || parsed.video_packets.empty()) {
    throw MovieError{"is not a WebM file with a video track"};
  }
  const TrackInfo& track = *parsed.video;
  if (track.codec != kAv1Codec) {
    throw MovieError{"has video that is not AV1"};
  }
  if (track.size.width <= 0 || track.size.height <= 0 ||
      static_cast<std::int64_t>(track.size.width) * track.size.height > kMaxMoviePixels) {
    throw MovieError{"has no picture size, or one that is too large"};
  }
  MovieData movie;
  movie.size = track.size;
  movie.av1_config = track.codec_private;
  movie.video = std::move(parsed.video_packets);
  const double first = movie.video.front().time;
  const double last = std::ranges::max(movie.video, {}, &MovieData::Packet::time).time;
  const auto count = static_cast<double>(movie.video.size());
  movie.duration =
      last + (movie.video.size() > 1 ? (last - first) / (count - 1) : kSingleFrameSeconds);
  if (parsed.audio && !parsed.audio_packets.empty()) {
    movie.audio = opus_to_ogg(*parsed.audio, parsed.audio_packets);
  }
  return movie;
}

std::shared_ptr<const MovieData> load_movie(const Vfs& vfs, std::string_view path) {
  if (!vfs.exists(path)) {
    throw MovieError{quoted_movie(path) + " not found"};
  }
  const std::optional<std::vector<std::byte>> bytes = vfs.read(path);
  if (!bytes) {
    throw MovieError{quoted_movie(path) + " cannot be read"};
  }
  try {
    MovieData movie = open_movie(*bytes);
    movie.path = std::string{path};
    return std::make_shared<const MovieData>(std::move(movie));
  } catch (const MovieError& error) {
    throw MovieError{quoted_movie(path) + " " + error.what()};
  }
}

std::string movie_problem(const Vfs& vfs, std::string_view path) {
  try {
    static_cast<void>(load_movie(vfs, path));
  } catch (const MovieError& error) {
    return error.what();
  }
  return {};
}

}  // namespace vnmaki
