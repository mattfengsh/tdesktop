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

#ifdef Q_OS_WIN
#include <d3d11.h>
#include <d3d11_1.h>
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

#ifdef Q_OS_WIN
const GUID NVVSR_D3D11_EXTENSION_GUID = { 0xD43CE1B3, 0x1F4B, 0x48AC, { 0xBA, 0xEE, 0xC3, 0xC2, 0x53, 0x75, 0xE6, 0xF7 } };

struct NVVSR_EXTENSION_PAYLOAD {
	UINT enable;
};

struct VsrContext {
	~VsrContext() {
		if (processor) processor->Release();
		if (enumerator) enumerator->Release();
		if (videoContext) videoContext->Release();
		if (videoDevice) videoDevice->Release();
		if (outputTexture) outputTexture->Release();
		if (outputView) outputView->Release();
		if (hwFramesCtx) av_buffer_unref(&hwFramesCtx);
	}
	ID3D11VideoDevice *videoDevice = nullptr;
	ID3D11VideoContext *videoContext = nullptr;
	ID3D11VideoProcessorEnumerator *enumerator = nullptr;
	ID3D11VideoProcessor *processor = nullptr;
	ID3D11Texture2D *outputTexture = nullptr;
	ID3D11VideoProcessorOutputView *outputView = nullptr;
	AVBufferRef *hwFramesCtx = nullptr;
	int width = 0;
	int height = 0;
	int outWidth = 0;
	int outHeight = 0;
};

bool ApplyVsrUpscale(
		Stream &stream,
		not_null<AVFrame*> decodedFrame,
		not_null<AVFrame*> transferredFrame) {
	if (decodedFrame->format != AV_PIX_FMT_D3D11) return false;

	auto frames_ctx = (AVHWFramesContext*)decodedFrame->hw_frames_ctx->data;
	auto device_ctx = (AVHWDeviceContext*)frames_ctx->device_ref->data;
	if (device_ctx->type != AV_HWDEVICE_TYPE_D3D11VA) return false;

	auto d3d11_device_ctx = (AVD3D11VADeviceContext*)device_ctx->hwctx;
	ID3D11Device *device = d3d11_device_ctx->device;
	ID3D11DeviceContext *context = d3d11_device_ctx->device_context;

	if (!stream.vsrContext) {
		stream.vsrContext = std::make_shared<VsrContext>();
	}
	auto vsr = static_cast<VsrContext*>(stream.vsrContext.get());

	if (decodedFrame->width >= 2560 || decodedFrame->height >= 1440) {
		return false; // Skip if already 2K or larger
	}

	int outWidth = decodedFrame->width;
	int outHeight = decodedFrame->height;

	double scaleW = 2560.0 / outWidth;
	double scaleH = 1440.0 / outHeight;
	double scale = std::min(scaleW, scaleH);

	if (scale > 1.0) {
		outWidth = static_cast<int>(std::round(outWidth * scale));
		outHeight = static_cast<int>(std::round(outHeight * scale));
		outWidth += outWidth % 2;   // NV12 requires even dimensions
		outHeight += outHeight % 2;
	} else {
		return false;
	}

	if (!vsr->videoDevice || vsr->width != decodedFrame->width || vsr->height != decodedFrame->height) {
		if (vsr->processor) { vsr->processor->Release(); vsr->processor = nullptr; }
		if (vsr->enumerator) { vsr->enumerator->Release(); vsr->enumerator = nullptr; }
		if (vsr->videoContext) { vsr->videoContext->Release(); vsr->videoContext = nullptr; }
		if (vsr->videoDevice) { vsr->videoDevice->Release(); vsr->videoDevice = nullptr; }
		if (vsr->outputTexture) { vsr->outputTexture->Release(); vsr->outputTexture = nullptr; }
		if (vsr->outputView) { vsr->outputView->Release(); vsr->outputView = nullptr; }
		if (vsr->hwFramesCtx) { av_buffer_unref(&vsr->hwFramesCtx); }

		vsr->width = decodedFrame->width;
		vsr->height = decodedFrame->height;
		vsr->outWidth = outWidth;
		vsr->outHeight = outHeight;

		HRESULT hr = device->QueryInterface(__uuidof(ID3D11VideoDevice), (void**)&vsr->videoDevice);
		if (FAILED(hr)) return false;

		hr = context->QueryInterface(__uuidof(ID3D11VideoContext), (void**)&vsr->videoContext);
		if (FAILED(hr)) return false;

		D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
		contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
		contentDesc.InputFrameRate = { 60, 1 };
		contentDesc.InputWidth = vsr->width;
		contentDesc.InputHeight = vsr->height;
		contentDesc.OutputWidth = vsr->outWidth;
		contentDesc.OutputHeight = vsr->outHeight;
		contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

		hr = vsr->videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &vsr->enumerator);
		if (FAILED(hr)) return false;

		hr = vsr->videoDevice->CreateVideoProcessor(vsr->enumerator, 0, &vsr->processor);
		if (FAILED(hr)) return false;

		NVVSR_EXTENSION_PAYLOAD payload = { 1 };
		vsr->videoContext->VideoProcessorSetStreamExtension(vsr->processor, 0, &NVVSR_D3D11_EXTENSION_GUID, sizeof(payload), &payload);

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
		if (FAILED(hr)) return false;

		D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc = {};
		ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
		ovDesc.Texture2D.MipSlice = 0;

		hr = vsr->videoDevice->CreateVideoProcessorOutputView(vsr->outputTexture, vsr->enumerator, &ovDesc, &vsr->outputView);
		if (FAILED(hr)) return false;

		vsr->hwFramesCtx = av_hwframe_ctx_alloc(frames_ctx->device_ref);
		if (!vsr->hwFramesCtx) return false;
		auto hwctx = (AVHWFramesContext*)vsr->hwFramesCtx->data;
		hwctx->format = AV_PIX_FMT_D3D11;
		hwctx->sw_format = AV_PIX_FMT_NV12;
		hwctx->width = vsr->outWidth;
		hwctx->height = vsr->outHeight;

		if (av_hwframe_ctx_init(vsr->hwFramesCtx) < 0) {
			av_buffer_unref(&vsr->hwFramesCtx);
			return false;
		}
	}

	ID3D11Texture2D *inputTexture = (ID3D11Texture2D*)decodedFrame->data[0];
	int inputIndex = (intptr_t)decodedFrame->data[1];

	ID3D11VideoProcessorInputView *inputView = nullptr;
	D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc = {};
	ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
	ivDesc.Texture2D.MipSlice = 0;
	ivDesc.Texture2D.ArraySlice = inputIndex;

	HRESULT hr = vsr->videoDevice->CreateVideoProcessorInputView(inputTexture, vsr->enumerator, &ivDesc, &inputView);
	if (FAILED(hr)) return false;

	D3D11_VIDEO_PROCESSOR_STREAM streamData = {};
	streamData.Enable = TRUE;
	streamData.OutputIndex = 0;
	streamData.InputFrameOrField = 0;
	streamData.PastFrames = 0;
	streamData.FutureFrames = 0;
	streamData.pInputSurface = inputView;

	vsr->videoContext->VideoProcessorSetStreamAutoProcessingMode(vsr->processor, 0, TRUE);

	RECT srcRect = { 0, 0, vsr->width, vsr->height };
	RECT dstRect = { 0, 0, vsr->outWidth, vsr->outHeight };
	vsr->videoContext->VideoProcessorSetStreamSourceRect(vsr->processor, 0, TRUE, &srcRect);
	vsr->videoContext->VideoProcessorSetStreamDestRect(vsr->processor, 0, TRUE, &dstRect);
	vsr->videoContext->VideoProcessorSetOutputTargetRect(vsr->processor, TRUE, &dstRect);

	hr = vsr->videoContext->VideoProcessorBlt(vsr->processor, vsr->outputView, 0, 1, &streamData);
	inputView->Release();
	if (FAILED(hr)) return false;

	AVFrame *tempFrame = av_frame_alloc();
	if (!tempFrame) return false;

	tempFrame->format = AV_PIX_FMT_D3D11;
	tempFrame->width = vsr->outWidth;
	tempFrame->height = vsr->outHeight;
	tempFrame->hw_frames_ctx = av_buffer_ref(vsr->hwFramesCtx);
	tempFrame->data[0] = (uint8_t*)vsr->outputTexture;
	tempFrame->data[1] = 0;

	int err = av_hwframe_transfer_data(transferredFrame, tempFrame, 0);
	av_frame_free(&tempFrame);

	return err == 0;
}
#endif

bool TransferFrame(
		Stream &stream,
		not_null<AVFrame*> decodedFrame,
		not_null<AVFrame*> transferredFrame) {
	Expects(decodedFrame->hw_frames_ctx != nullptr);

#ifdef Q_OS_WIN
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
