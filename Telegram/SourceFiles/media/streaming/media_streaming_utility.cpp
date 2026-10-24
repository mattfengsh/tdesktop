/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_utility.h"

#include "media/streaming/media_streaming_common.h"
#include "ui/image/image_prepare.h"
#include "ui/painter.h"
#include "ffmpeg/ffmpeg_utility.h"

#include <cmath>

#if defined(Q_OS_WIN) && defined(_WIN64) && (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#endif
namespace Media {
namespace Streaming {
namespace {

constexpr auto kSkipInvalidDataPackets = 10;

} // namespace

crl::time FramePosition(const Stream &stream) {
	const auto pts = !stream.decodedFrame
		? AV_NOPTS_VALUE
		: (stream.decodedFrame->best_effort_timestamp != AV_NOPTS_VALUE)
		? stream.decodedFrame->best_effort_timestamp
		: (stream.decodedFrame->pts != AV_NOPTS_VALUE)
		? stream.decodedFrame->pts
		: stream.decodedFrame->pkt_dts;
	const auto result = FFmpeg::PtsToTime(pts, stream.timeBase);

	// Sometimes the result here may be larger than the stream duration.
	return (stream.duration == kDurationUnavailable)
		? result
		: std::min(result, stream.duration);
}

FFmpeg::AvErrorWrap ProcessPacket(Stream &stream, FFmpeg::Packet &&packet) {
	Expects(stream.codec != nullptr);

	auto error = FFmpeg::AvErrorWrap();

	const auto native = &packet.fields();
	const auto guard = gsl::finally([
		&,
		size = native->size,
		data = native->data
	] {
		native->size = size;
		native->data = data;
		packet = FFmpeg::Packet();
	});

	error = avcodec_send_packet(
		stream.codec.get(),
		native->data ? native : nullptr); // Drain on eof.
	if (error) {
		LogError(u"avcodec_send_packet"_q, error);
		if (error.code() == AVERROR_INVALIDDATA
			// There is a sample voice message where skipping such packet
			// results in a crash (read_access to nullptr) in swr_convert().
			&& stream.codec->codec_id != AV_CODEC_ID_OPUS) {
			if (++stream.invalidDataPackets < kSkipInvalidDataPackets) {
				return FFmpeg::AvErrorWrap(); // Try to skip a bad packet.
			}
		}
	} else {
		stream.invalidDataPackets = 0;
	}
	return error;
}

FFmpeg::AvErrorWrap ReadNextFrame(Stream &stream) {
	Expects(stream.decodedFrame != nullptr);

	auto error = FFmpeg::AvErrorWrap();

	do {
		error = avcodec_receive_frame(
			stream.codec.get(),
			stream.decodedFrame.get());
		if (!error
			|| error.code() != AVERROR(EAGAIN)
			|| stream.queue.empty()) {
			return error;
		}

		error = ProcessPacket(stream, std::move(stream.queue.front()));
		stream.queue.pop_front();
	} while (!error);

	return error;
}

bool GoodForRequest(
		const QImage &image,
		bool hasAlpha,
		int rotation,
		const FrameRequest &request) {
	if (image.isNull()
		|| (hasAlpha && !request.keepAlpha)
		|| request.colored.alpha() != 0) {
		return false;
	} else if (!request.blurredBackground && request.resize.isEmpty()) {
		return true;
	} else if (rotation != 0) {
		return false;
	} else if (!request.rounding.empty() || !request.mask.isNull()) {
		return false;
	}
	const auto size = request.blurredBackground
		? request.outer
		: request.resize;
	return (size == request.outer) && (size == image.size());
}

#if defined(Q_OS_WIN) && defined(_WIN64) && (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
constexpr GUID kNvidiaPPEInterfaceGUID = {
	0xd43ce1b3,
	0x1f4b,
	0x48ac,
	{ 0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7 }
};
constexpr UINT kStreamExtensionVersionV1 = 0x1;
constexpr UINT kStreamExtensionMethodSuperResolution = 0x2;

struct VsrStreamExtensionInfo {
	UINT version = kStreamExtensionVersionV1;
	UINT method = kStreamExtensionMethodSuperResolution;
	UINT enable = 1;
};

struct VsrContext {
	~VsrContext() {
		release();
	}

	void release() {
		if (inputView) {
			inputView->Release();
			inputView = nullptr;
		}
		if (outputView) {
			outputView->Release();
			outputView = nullptr;
		}
		if (outputTexture) {
			outputTexture->Release();
			outputTexture = nullptr;
		}
		if (stagingTexture) {
			stagingTexture->Release();
			stagingTexture = nullptr;
		}
		if (processor) {
			processor->Release();
			processor = nullptr;
		}
		if (enumerator) {
			enumerator->Release();
			enumerator = nullptr;
		}
		if (videoContext) {
			videoContext->Release();
			videoContext = nullptr;
		}
		if (videoDevice) {
			videoDevice->Release();
			videoDevice = nullptr;
		}
		width = 0;
		height = 0;
		outWidth = 0;
		outHeight = 0;
		lastInputTexture = nullptr;
		lastInputIndex = -1;
	}

	ID3D11VideoDevice *videoDevice = nullptr;
	ID3D11VideoContext *videoContext = nullptr;
	ID3D11VideoProcessorEnumerator *enumerator = nullptr;
	ID3D11VideoProcessor *processor = nullptr;
	ID3D11Texture2D *outputTexture = nullptr;
	ID3D11Texture2D *stagingTexture = nullptr;
	ID3D11VideoProcessorOutputView *outputView = nullptr;
	ID3D11VideoProcessorInputView *inputView = nullptr;

	ID3D11Texture2D *lastInputTexture = nullptr;
	int lastInputIndex = -1;

	int width = 0;
	int height = 0;
	int outWidth = 0;
	int outHeight = 0;
};

bool ApplyVsrUpscale(
		Stream &stream,
		not_null<AVFrame*> decodedFrame,
		not_null<AVFrame*> transferredFrame) {
	if (stream.vsrDisabled) {
		return false;
	}
	if (decodedFrame->format != AV_PIX_FMT_D3D11 || !decodedFrame->hw_frames_ctx) {
		return false;
	}

	const auto frames_ctx = reinterpret_cast<AVHWFramesContext*>(
		decodedFrame->hw_frames_ctx->data);
	if (!frames_ctx || !frames_ctx->device_ref) {
		return false;
	}
	const auto device_ctx = reinterpret_cast<AVHWDeviceContext*>(
		frames_ctx->device_ref->data);
	if (!device_ctx || device_ctx->type != AV_HWDEVICE_TYPE_D3D11VA) {
		return false;
	}

	const auto d3d11_device_ctx = reinterpret_cast<AVD3D11VADeviceContext*>(
		device_ctx->hwctx);
	if (!d3d11_device_ctx) {
		return false;
	}
	ID3D11Device *device = d3d11_device_ctx->device;
	ID3D11DeviceContext *context = d3d11_device_ctx->device_context;
	if (!device || !context) {
		return false;
	}

	const int inWidth = decodedFrame->width;
	const int inHeight = decodedFrame->height;
	if (inWidth <= 0 || inHeight <= 0) {
		return false;
	}

	const bool isPortrait = (inHeight > inWidth);
	const int maxBoundW = isPortrait ? 1440 : 2560;
	const int maxBoundH = isPortrait ? 2560 : 1440;

	if (inWidth >= maxBoundW && inHeight >= maxBoundH) {
		return false;
	}

	const double scaleW = double(maxBoundW) / inWidth;
	const double scaleH = double(maxBoundH) / inHeight;
	const double scale = std::min(scaleW, scaleH);
	if (scale <= 1.0) {
		return false;
	}

	int outWidth = static_cast<int>(std::round(inWidth * scale));
	int outHeight = static_cast<int>(std::round(inHeight * scale));
	outWidth += (outWidth % 2);
	outHeight += (outHeight % 2);

	if (!stream.vsrContext) {
		stream.vsrContext = std::make_shared<VsrContext>();
	}
	auto vsr = static_cast<VsrContext*>(stream.vsrContext.get());

	if (!vsr->videoDevice || vsr->width != inWidth || vsr->height != inHeight || vsr->outWidth != outWidth || vsr->outHeight != outHeight) {
		vsr->release();

		IDXGIDevice *dxgiDevice = nullptr;
		HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
		if (FAILED(hr) || !dxgiDevice) {
			stream.vsrDisabled = true;
			return false;
		}
		IDXGIAdapter *dxgiAdapter = nullptr;
		hr = dxgiDevice->GetAdapter(&dxgiAdapter);
		dxgiDevice->Release();
		if (FAILED(hr) || !dxgiAdapter) {
			stream.vsrDisabled = true;
			return false;
		}
		DXGI_ADAPTER_DESC adapterDesc = {};
		hr = dxgiAdapter->GetDesc(&adapterDesc);
		dxgiAdapter->Release();
		if (FAILED(hr) || adapterDesc.VendorId != 0x10DE) {
			stream.vsrDisabled = true;
			return false;
		}

		vsr->width = inWidth;
		vsr->height = inHeight;
		vsr->outWidth = outWidth;
		vsr->outHeight = outHeight;

		hr = device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&vsr->videoDevice));
		if (FAILED(hr)) {
			vsr->release();
			stream.vsrDisabled = true;
			return false;
		}

		hr = context->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&vsr->videoContext));
		if (FAILED(hr)) {
			vsr->release();
			stream.vsrDisabled = true;
			return false;
		}

		D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
		contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
		contentDesc.InputFrameRate = { 60, 1 };
		contentDesc.InputWidth = vsr->width;
		contentDesc.InputHeight = vsr->height;
		contentDesc.OutputWidth = vsr->outWidth;
		contentDesc.OutputHeight = vsr->outHeight;
		contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

		hr = vsr->videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &vsr->enumerator);
		if (FAILED(hr)) {
			vsr->release();
			stream.vsrDisabled = true;
			return false;
		}

		UINT inFlags = 0;
		hr = vsr->enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &inFlags);
		if (FAILED(hr) || !(inFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
			vsr->release();
			stream.vsrDisabled = true;
			return false;
		}

		hr = vsr->videoDevice->CreateVideoProcessor(vsr->enumerator, 0, &vsr->processor);
		if (FAILED(hr)) {
			vsr->release();
			stream.vsrDisabled = true;
			return false;
		}

		VsrStreamExtensionInfo extensionPayload;
		hr = vsr->videoContext->VideoProcessorSetStreamExtension(
			vsr->processor,
			0,
			&kNvidiaPPEInterfaceGUID,
			sizeof(extensionPayload),
			&extensionPayload);
		if (FAILED(hr)) {
			vsr->release();
			stream.vsrDisabled = true;
			return false;
		}

		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = vsr->outWidth;
		texDesc.Height = vsr->outHeight;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_NV12;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

		hr = device->CreateTexture2D(&texDesc, nullptr, &vsr->outputTexture);
		if (FAILED(hr)) {
			vsr->release();
			stream.vsrDisabled = true;
			return false;
		}

		D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc = {};
		ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
		ovDesc.Texture2D.MipSlice = 0;

		hr = vsr->videoDevice->CreateVideoProcessorOutputView(
			vsr->outputTexture,
			vsr->enumerator,
			&ovDesc,
			&vsr->outputView);
		if (FAILED(hr)) {
			vsr->release();
			stream.vsrDisabled = true;
			return false;
		}

		D3D11_TEXTURE2D_DESC stagingDesc = {};
		stagingDesc.Width = vsr->outWidth;
		stagingDesc.Height = vsr->outHeight;
		stagingDesc.MipLevels = 1;
		stagingDesc.ArraySize = 1;
		stagingDesc.Format = DXGI_FORMAT_NV12;
		stagingDesc.SampleDesc.Count = 1;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		hr = device->CreateTexture2D(&stagingDesc, nullptr, &vsr->stagingTexture);
		if (FAILED(hr)) {
			vsr->release();
			stream.vsrDisabled = true;
			return false;
		}
	}

	ID3D11Texture2D *inputTexture = reinterpret_cast<ID3D11Texture2D*>(decodedFrame->data[0]);
	const int inputIndex = static_cast<int>(reinterpret_cast<intptr_t>(decodedFrame->data[1]));
	if (!inputTexture) {
		return false;
	}

	if (!vsr->inputView || inputTexture != vsr->lastInputTexture || inputIndex != vsr->lastInputIndex) {
		if (vsr->inputView) {
			vsr->inputView->Release();
			vsr->inputView = nullptr;
		}
		D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc = {};
		ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
		ivDesc.Texture2D.MipSlice = 0;
		ivDesc.Texture2D.ArraySlice = inputIndex;

		HRESULT hr = vsr->videoDevice->CreateVideoProcessorInputView(
			inputTexture,
			vsr->enumerator,
			&ivDesc,
			&vsr->inputView);
		if (FAILED(hr)) {
			return false;
		}
		vsr->lastInputTexture = inputTexture;
		vsr->lastInputIndex = inputIndex;
	}

	D3D11_VIDEO_PROCESSOR_STREAM streamData = {};
	streamData.Enable = TRUE;
	streamData.OutputIndex = 0;
	streamData.InputFrameOrField = 0;
	streamData.PastFrames = 0;
	streamData.FutureFrames = 0;
	streamData.pInputSurface = vsr->inputView;

	RECT srcRect = { 0, 0, vsr->width, vsr->height };
	RECT dstRect = { 0, 0, vsr->outWidth, vsr->outHeight };
	vsr->videoContext->VideoProcessorSetStreamSourceRect(vsr->processor, 0, TRUE, &srcRect);
	vsr->videoContext->VideoProcessorSetStreamDestRect(vsr->processor, 0, TRUE, &dstRect);
	vsr->videoContext->VideoProcessorSetOutputTargetRect(vsr->processor, TRUE, &dstRect);

	HRESULT hr = vsr->videoContext->VideoProcessorBlt(
		vsr->processor,
		vsr->outputView,
		0,
		1,
		&streamData);
	if (FAILED(hr)) {
		return false;
	}

	context->CopyResource(vsr->stagingTexture, vsr->outputTexture);

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	hr = context->Map(vsr->stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr)) {
		return false;
	}

	av_frame_unref(transferredFrame);
	transferredFrame->format = AV_PIX_FMT_NV12;
	transferredFrame->width = vsr->outWidth;
	transferredFrame->height = vsr->outHeight;
	if (av_frame_get_buffer(transferredFrame, 32) < 0) {
		context->Unmap(vsr->stagingTexture, 0);
		return false;
	}

	const uint8_t *srcY = reinterpret_cast<const uint8_t*>(mapped.pData);
	uint8_t *dstY = transferredFrame->data[0];
	const int widthBytes = vsr->outWidth;
	for (int y = 0; y < vsr->outHeight; ++y) {
		memcpy(dstY, srcY, widthBytes);
		srcY += mapped.RowPitch;
		dstY += transferredFrame->linesize[0];
	}

	const uint8_t *srcUV = reinterpret_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(mapped.RowPitch) * vsr->outHeight;
	uint8_t *dstUV = transferredFrame->data[1];
	const int uvHeight = vsr->outHeight / 2;
	for (int y = 0; y < uvHeight; ++y) {
		memcpy(dstUV, srcUV, widthBytes);
		srcUV += mapped.RowPitch;
		dstUV += transferredFrame->linesize[1];
	}

	context->Unmap(vsr->stagingTexture, 0);

	transferredFrame->pts = decodedFrame->pts;
	transferredFrame->pkt_dts = decodedFrame->pkt_dts;
	transferredFrame->best_effort_timestamp = decodedFrame->best_effort_timestamp;
	transferredFrame->sample_aspect_ratio = decodedFrame->sample_aspect_ratio;
	transferredFrame->color_primaries = decodedFrame->color_primaries;
	transferredFrame->color_trc = decodedFrame->color_trc;
	transferredFrame->colorspace = decodedFrame->colorspace;
	transferredFrame->color_range = decodedFrame->color_range;
	transferredFrame->chroma_location = decodedFrame->chroma_location;

	return true;
}
#endif

bool TransferFrame(
		Stream &stream,
		not_null<AVFrame*> decodedFrame,
		not_null<AVFrame*> transferredFrame) {
	Expects(decodedFrame->hw_frames_ctx != nullptr);

#if defined(Q_OS_WIN) && defined(_WIN64) && (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
	if (ApplyVsrUpscale(stream, decodedFrame, transferredFrame)) {
		FFmpeg::ClearFrameMemory(decodedFrame);
		return true;
	}
#endif

	const auto error = FFmpeg::AvErrorWrap(
		av_hwframe_transfer_data(transferredFrame, decodedFrame, 0));
	if (error) {
		LogError(u"av_hwframe_transfer_data"_q, error);
		return false;
	}
	FFmpeg::ClearFrameMemory(decodedFrame);
	return true;
}

QImage ConvertFrame(
		Stream &stream,
		not_null<AVFrame*> frame,
		QSize resize,
		QImage storage) {
	const auto frameSize = QSize(frame->width, frame->height);
	if (frameSize.isEmpty()) {
		LOG(("Streaming Error: Bad frame size %1,%2"
			).arg(frameSize.width()
			).arg(frameSize.height()));
		return QImage();
	} else if (!FFmpeg::FrameHasData(frame)) {
		LOG(("Streaming Error: Bad frame data."));
		return QImage();
	}
	if (resize.isEmpty()) {
		resize = frameSize;
	} else if (FFmpeg::RotationSwapWidthHeight(stream.rotation)) {
		resize.transpose();
	}

	if (!FFmpeg::GoodStorageForFrame(storage, resize)) {
		storage = FFmpeg::CreateFrameStorage(resize);
		if (storage.isNull()) {
			return QImage();
		}
	}

	const auto format = AV_PIX_FMT_BGRA;
	const auto hasDesiredFormat = (frame->format == format);
	if (hasDesiredFormat
		&& frameSize == storage.size()
		&& frame->linesize[0] > 0) {
		static_assert(sizeof(uint32) == FFmpeg::kPixelBytesSize);
		auto to = reinterpret_cast<uint32*>(storage.bits());
		auto from = reinterpret_cast<const uint32*>(frame->data[0]);
		const auto deltaTo = (storage.bytesPerLine() / sizeof(uint32))
			- storage.width();
		const auto deltaFrom = (frame->linesize[0] / sizeof(uint32))
			- frame->width;
		for ([[maybe_unused]] const auto y : ranges::views::ints(0, frame->height)) {
			for ([[maybe_unused]] const auto x : ranges::views::ints(0, frame->width)) {
				// Wipe out possible alpha values.
				*to++ = 0xFF000000U | *from++;
			}
			to += deltaTo;
			from += deltaFrom;
		}
	} else {
		stream.swscale = MakeSwscalePointer(
			frame,
			resize,
			&stream.swscale);
		if (!stream.swscale) {
			return QImage();
		}

		// AV_NUM_DATA_POINTERS defined in AVFrame struct
		uint8_t *data[AV_NUM_DATA_POINTERS] = { storage.bits(), nullptr };
		int linesize[AV_NUM_DATA_POINTERS] = { int(storage.bytesPerLine()), 0 };

		sws_scale(
			stream.swscale.get(),
			frame->data,
			frame->linesize,
			0,
			frame->height,
			data,
			linesize);

		if (frame->format == AV_PIX_FMT_YUVA420P) {
			FFmpeg::PremultiplyInplace(storage);
		}
	}

	FFmpeg::ClearFrameMemory(frame);
	return storage;
}

FrameYUV ExtractYUV(Stream &stream, AVFrame *frame) {
	return {
		.size = { frame->width, frame->height },
		.chromaSize = {
			AV_CEIL_RSHIFT(frame->width, 1), // SWScale does that.
			AV_CEIL_RSHIFT(frame->height, 1)
		},
		.y = { .data = frame->data[0], .stride = frame->linesize[0] },
		.u = { .data = frame->data[1], .stride = frame->linesize[1] },
		.v = { .data = frame->data[2], .stride = frame->linesize[2] },
	};
}

void PaintFrameOuter(QPainter &p, const QRect &inner, QSize outer) {
	const auto left = inner.x();
	const auto right = outer.width() - inner.width() - left;
	const auto top = inner.y();
	const auto bottom = outer.height() - inner.height() - top;
	if (left > 0) {
		p.fillRect(0, 0, left, outer.height(), st::imageBg);
	}
	if (right > 0) {
		p.fillRect(
			outer.width() - right,
			0,
			right,
			outer.height(),
			st::imageBg);
	}
	if (top > 0) {
		p.fillRect(left, 0, inner.width(), top, st::imageBg);
	}
	if (bottom > 0) {
		p.fillRect(
			left,
			outer.height() - bottom,
			inner.width(),
			bottom,
			st::imageBg);
	}
}

void PaintFrameInner(
		QPainter &p,
		QRect to,
		const QImage &original,
		bool alpha,
		int rotation) {
	const auto rotated = [](QRect rect, int rotation) {
		switch (rotation) {
		case 0: return rect;
		case 90: return QRect(
			rect.y(),
			-rect.x() - rect.width(),
			rect.height(),
			rect.width());
		case 180: return QRect(
			-rect.x() - rect.width(),
			-rect.y() - rect.height(),
			rect.width(),
			rect.height());
		case 270: return QRect(
			-rect.y() - rect.height(),
			rect.x(),
			rect.height(),
			rect.width());
		}
		Unexpected("Rotation in PaintFrameInner.");
	};

	PainterHighQualityEnabler hq(p);
	if (rotation) {
		p.rotate(rotation);
	}
	const auto rect = rotated(to, rotation);
	if (alpha) {
		p.fillRect(rect, Qt::white);
	}
	p.drawImage(rect, original);
}

QImage PrepareBlurredBackground(QSize outer, QImage frame) {
	const auto bsize = frame.size();
	const auto copyw = std::min(
		bsize.width(),
		std::max(outer.width() * bsize.height() / outer.height(), 1));
	const auto copyh = std::min(
		bsize.height(),
		std::max(outer.height() * bsize.width() / outer.width(), 1));
	auto copy = (bsize == QSize(copyw, copyh))
		? std::move(frame)
		: frame.copy(
			(bsize.width() - copyw) / 2,
			(bsize.height() - copyh) / 2,
			copyw,
			copyh);
	auto scaled = (copy.width() <= 100 && copy.height() <= 100)
		? std::move(copy)
		: copy.scaled(40, 40, Qt::KeepAspectRatio, Qt::FastTransformation);
	return Images::Blur(std::move(scaled), true);
}

void FillBlurredBackground(QPainter &p, QSize outer, QImage bg) {
	auto hq = PainterHighQualityEnabler(p);
	const auto rect = QRect(QPoint(), outer);
	const auto ratio = p.device()->devicePixelRatio();
	p.drawImage(
		rect,
		PrepareBlurredBackground(outer * ratio, std::move(bg)));
	p.fillRect(rect, QColor(0, 0, 0, 48));
}

void PaintFrameContent(
		QPainter &p,
		const QImage &original,
		bool hasAlpha,
		const AVRational &aspect,
		int rotation,
		const FrameRequest &request) {
	const auto outer = request.outer;
	const auto full = request.outer.isEmpty() ? original.size() : outer;
	const auto deAlpha = hasAlpha && !request.keepAlpha;
	const auto resize = request.blurredBackground
		? DecideVideoFrameResize(
			outer,
			FFmpeg::TransposeSizeByRotation(
				FFmpeg::CorrectByAspect(original.size(), aspect), rotation))
		: ExpandDecision{ request.resize.isEmpty()
			? original.size()
			: request.resize };
	const auto size = resize.result;
	const auto target = QRect(
		(full.width() - size.width()) / 2,
		(full.height() - size.height()) / 2,
		size.width(),
		size.height());
	if (request.blurredBackground) {
		if (!resize.expanding) {
			FillBlurredBackground(p, full, original);
		}
	} else if (!hasAlpha || !request.keepAlpha) {
		PaintFrameOuter(p, target, full);
	}
	PaintFrameInner(p, target, original, deAlpha, rotation);
}

void ApplyFrameRounding(QImage &storage, const FrameRequest &request) {
	if (!request.mask.isNull()) {
		auto p = QPainter(&storage);
		p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
		p.drawImage(
			QRect(QPoint(), storage.size() / storage.devicePixelRatio()),
			request.mask);
	} else if (!request.rounding.empty()) {
		storage = Images::Round(std::move(storage), request.rounding);
	}
}

ExpandDecision DecideFrameResize(
		QSize outer,
		QSize original,
		int minVisibleNominator,
		int minVisibleDenominator) {
	if (outer.isEmpty()) {
		// Often "expanding" means that we don't need to fill the background.
		return { .result = original, .expanding = true };
	}
	const auto big = original.scaled(outer, Qt::KeepAspectRatioByExpanding);
	if ((big.width() <= outer.width())
		&& (big.height() * minVisibleNominator
			<= outer.height() * minVisibleDenominator)) {
		return { .result = big, .expanding = true };
	}
	return { .result = original.scaled(outer, Qt::KeepAspectRatio) };
}

bool FrameResizeMayExpand(
		QSize outer,
		QSize original,
		int minVisibleNominator,
		int minVisibleDenominator) {
	const auto min = std::min({
		outer.width(),
		outer.height(),
		original.width(),
		original.height(),
	});
	// Count for: (nominator / denominator) - (1 / min).
	// In case the result is less than 1 / 2, just return.
	if (2 * minVisibleNominator * min
		< 2 * minVisibleDenominator + minVisibleDenominator * min) {
		return false;
	}
	return DecideFrameResize(
		outer,
		original,
		minVisibleNominator * min - minVisibleDenominator,
		minVisibleDenominator * min).expanding;
}

ExpandDecision DecideVideoFrameResize(QSize outer, QSize original) {
	return DecideFrameResize(outer, original, 1, 2);
}

QSize CalculateResizeFromOuter(QSize outer, QSize original) {
	return DecideVideoFrameResize(outer, original).result;
}

QImage PrepareByRequest(
		const QImage &original,
		bool hasAlpha,
		const AVRational &aspect,
		int rotation,
		const FrameRequest &request,
		QImage storage) {
	Expects(!request.outer.isEmpty() || hasAlpha);

	const auto outer = request.outer.isEmpty()
		? original.size()
		: request.outer;
	if (!FFmpeg::GoodStorageForFrame(storage, outer)) {
		storage = FFmpeg::CreateFrameStorage(outer);
		if (storage.isNull()) {
			return QImage();
		}
	}

	if (hasAlpha && request.keepAlpha) {
		storage.fill(Qt::transparent);
	}

	QPainter p(&storage);
	PaintFrameContent(p, original, hasAlpha, aspect, rotation, request);
	p.end();

	ApplyFrameRounding(storage, request);
	if (request.colored.alpha() != 0) {
		storage = Images::Colored(std::move(storage), request.colored);
	}
	return storage;
}

} // namespace Streaming
} // namespace Media
