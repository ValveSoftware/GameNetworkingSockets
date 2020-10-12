#include(FetchContent)
include(TestBigEndian)
include(CheckSymbolExists)

find_package(OpenSSL REQUIRED)
find_package(Protobuf REQUIRED)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(absl QUIET)

if(NOT absl_FOUND)
	set(STEAMWEBRTC_ABSL_SOURCE submodule PARENT_SCOPE)

	set(abseil_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../abseil)
	if(NOT EXISTS ${abseil_SOURCE_DIR}/CMakeLists.txt)
		message(FATAL_ERROR "abeil submodule not initialized.\n(Try 'git submodule update src/external/abseil')" )
	endif()
	add_subdirectory(${abseil_SOURCE_DIR} ${CMAKE_BINARY_DIR}/abseil EXCLUDE_FROM_ALL)
	if(LTO)
		set_property(DIRECTORY ${abseil_SOURCE_DIR} PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
	endif()
else()
	set(STEAMWEBRTC_ABSL_SOURCE package PARENT_SCOPE)
endif()

set(webrtc_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../webrtc)
#FIXME set(webrtc_BUILD_DIR ????? )
if(NOT EXISTS ${webrtc_SOURCE_DIR}/rtc_base)
	message(FATAL_ERROR "webrtc submodule not initialized.\n(Try: 'git submodule update src/external/webrtc')" )
endif()

include (CheckTypeSize)
CHECK_TYPE_SIZE("void*" CMAKE_SIZEOF_VOID_P)
TEST_BIG_ENDIAN(IS_BIG_ENDIAN)

# Slam some defines to override the values set in rtc_base/system/arch.h
# FIXME - What platforms does this need to be set on?  Let's not do this
#         unless we are on a platform not currently handled.
# endian and cpu detect : for ppc64le and s390x, else
#add_definitions( -DRTC_BASE_SYSTEM_ARCH_H_ )
#if(CMAKE_SIZEOF_VOID_P EQUAL 8)
#	add_definitions( -DWEBRTC_ARCH_64_BITS )
#elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
#	add_definitions( -DWEBRTC_ARCH_32_BITS )
#endif()
#if(IS_BIG_ENDIAN)
#	# This is actually not a thing used in WebRTC code, but I suppose
#	# it cannot hurt to define it.
#	add_definitions( -DWEBRTC_ARCH_BIG_ENDIAN )
#else()
#	add_definitions( -DWEBRTC_ARCH_LITTLE_ENDIAN )
#endif()

if(IS_BIG_ENDIAN)
	# There are no big endian architectures defined in WebRTC's arch.h file, so
	# let's just error out early because the user would need to do some work to
	# add the platform or verify it even works on there.
	message(FATAL_ERROR "WebRTC does not have your architecture defined in arch.h")
endif()

if(WIN32)
	add_definitions(
		-DWEBRTC_WIN
		-DNOMINMAX
		-DWIN32_LEAN_AND_MEAN
		-D_WINSOCKAPI_
		)
		add_compile_options(
			/wd4715   # src\external\webrtc\pc\dtls_transport.cc(34) : warning C4715: 'webrtc::`anonymous namespace'::TranslateState': not all control paths return a value
			/wd4005   # C:\Program Files (x86)\Windows Kits\10\include\10.0.18362.0\um\winsock2.h(17): warning C4005: '_WINSOCKAPI_': macro redefinition
			/wd4996   # src\external\webrtc\rtc_base\win32.cc(324): warning C4996: 'GetVersionExA': was declared deprecated
			/wd4530   # C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Tools\MSVC\14.25.28610\include\ostream(281): warning C4530: C++ exception handler used, but unwind semantics are not enabled. Specify /EHsc
		)
else(WIN32)
	set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}  -Wl,--no-undefined ")
	add_definitions(
		-DWEBRTC_POSIX
		-DWEBRTC_LINUX
		)
endif(WIN32)

add_library(webrtc-lite STATIC
	${webrtc_SOURCE_DIR}/api/adaptation/resource.cc
	${webrtc_SOURCE_DIR}/api/adaptation/resource.h
	${webrtc_SOURCE_DIR}/api/array_view.h
	${webrtc_SOURCE_DIR}/api/async_resolver_factory.h
	#${webrtc_SOURCE_DIR}/api/audio_codecs/audio_codec_pair_id.cc
	#${webrtc_SOURCE_DIR}/api/audio_codecs/audio_codec_pair_id.h
	#${webrtc_SOURCE_DIR}/api/audio_codecs/audio_decoder_factory.h
	#${webrtc_SOURCE_DIR}/api/audio_codecs/audio_decoder.cc
	#${webrtc_SOURCE_DIR}/api/audio_codecs/audio_decoder.h
	#${webrtc_SOURCE_DIR}/api/audio_codecs/audio_encoder_factory.h
	#${webrtc_SOURCE_DIR}/api/audio_codecs/audio_encoder.cc
	#${webrtc_SOURCE_DIR}/api/audio_codecs/audio_encoder.h
	#${webrtc_SOURCE_DIR}/api/audio_codecs/audio_format.cc
	#${webrtc_SOURCE_DIR}/api/audio_codecs/audio_format.h
	#${webrtc_SOURCE_DIR}/api/audio_options.cc
	#${webrtc_SOURCE_DIR}/api/audio_options.h
	#${webrtc_SOURCE_DIR}/api/audio/audio_frame.cc
	#${webrtc_SOURCE_DIR}/api/audio/audio_frame.h
	#${webrtc_SOURCE_DIR}/api/audio/audio_mixer.h
	#${webrtc_SOURCE_DIR}/api/audio/channel_layout.cc
	#${webrtc_SOURCE_DIR}/api/audio/channel_layout.h
	#${webrtc_SOURCE_DIR}/api/audio/echo_canceller3_config.cc
	#${webrtc_SOURCE_DIR}/api/audio/echo_canceller3_config.h
	#${webrtc_SOURCE_DIR}/api/audio/echo_control.h
	#${webrtc_SOURCE_DIR}/api/call/audio_sink.h
	#${webrtc_SOURCE_DIR}/api/call/bitrate_allocation.h
	#${webrtc_SOURCE_DIR}/api/call/transport.cc
	#${webrtc_SOURCE_DIR}/api/call/transport.h
	${webrtc_SOURCE_DIR}/api/candidate.cc
	${webrtc_SOURCE_DIR}/api/candidate.h
	${webrtc_SOURCE_DIR}/api/crypto_params.h
	${webrtc_SOURCE_DIR}/api/crypto/crypto_options.cc
	${webrtc_SOURCE_DIR}/api/crypto/crypto_options.h
	${webrtc_SOURCE_DIR}/api/crypto/frame_decryptor_interface.h
	${webrtc_SOURCE_DIR}/api/crypto/frame_encryptor_interface.h
	${webrtc_SOURCE_DIR}/api/dtls_transport_interface.cc
	${webrtc_SOURCE_DIR}/api/dtls_transport_interface.h
	${webrtc_SOURCE_DIR}/api/dtmf_sender_interface.h
	${webrtc_SOURCE_DIR}/api/fec_controller_override.h
	${webrtc_SOURCE_DIR}/api/fec_controller.h
	${webrtc_SOURCE_DIR}/api/frame_transformer_interface.h
	${webrtc_SOURCE_DIR}/api/function_view.h
	${webrtc_SOURCE_DIR}/api/ice_transport_interface.h
	#${webrtc_SOURCE_DIR}/api/jsep_ice_candidate.cc
	#${webrtc_SOURCE_DIR}/api/jsep_ice_candidate.cc
	#${webrtc_SOURCE_DIR}/api/jsep_ice_candidate.h
	#${webrtc_SOURCE_DIR}/api/jsep_session_description.h
	#${webrtc_SOURCE_DIR}/api/jsep.cc
	#${webrtc_SOURCE_DIR}/api/jsep.h
	#${webrtc_SOURCE_DIR}/api/media_stream_interface.cc
	#${webrtc_SOURCE_DIR}/api/media_stream_interface.h
	#${webrtc_SOURCE_DIR}/api/media_types.cc
	#${webrtc_SOURCE_DIR}/api/media_types.h
	${webrtc_SOURCE_DIR}/api/neteq/neteq_factory.h
	${webrtc_SOURCE_DIR}/api/neteq/neteq.cc
	${webrtc_SOURCE_DIR}/api/neteq/neteq.h
	${webrtc_SOURCE_DIR}/api/network_state_predictor.h
	${webrtc_SOURCE_DIR}/api/packet_socket_factory.h
	${webrtc_SOURCE_DIR}/api/priority.h
	${webrtc_SOURCE_DIR}/api/proxy.cc
	${webrtc_SOURCE_DIR}/api/proxy.h
	${webrtc_SOURCE_DIR}/api/ref_counted_base.h
	${webrtc_SOURCE_DIR}/api/rtc_error.cc
	${webrtc_SOURCE_DIR}/api/rtc_error.h
	#${webrtc_SOURCE_DIR}/api/rtc_event_log_output.h
	#${webrtc_SOURCE_DIR}/api/rtc_event_log/rtc_event_log_factory_interface.h
	#${webrtc_SOURCE_DIR}/api/rtc_event_log/rtc_event_log_factory.cc
	#${webrtc_SOURCE_DIR}/api/rtc_event_log/rtc_event_log_factory.cc
	#${webrtc_SOURCE_DIR}/api/rtc_event_log/rtc_event_log_factory.h
	#${webrtc_SOURCE_DIR}/api/rtc_event_log/rtc_event_log.cc
	#${webrtc_SOURCE_DIR}/api/rtc_event_log/rtc_event_log.h
	${webrtc_SOURCE_DIR}/api/rtc_event_log/rtc_event.cc
	${webrtc_SOURCE_DIR}/api/rtc_event_log/rtc_event.h
	#${webrtc_SOURCE_DIR}/api/rtp_headers.cc
	#${webrtc_SOURCE_DIR}/api/rtp_headers.h
	#${webrtc_SOURCE_DIR}/api/rtp_packet_info.cc
	#${webrtc_SOURCE_DIR}/api/rtp_packet_info.h
	#${webrtc_SOURCE_DIR}/api/rtp_packet_infos.h
	#${webrtc_SOURCE_DIR}/api/rtp_parameters.cc
	#${webrtc_SOURCE_DIR}/api/rtp_parameters.h
	#${webrtc_SOURCE_DIR}/api/rtp_receiver_interface.cc
	#${webrtc_SOURCE_DIR}/api/rtp_receiver_interface.h
	#${webrtc_SOURCE_DIR}/api/rtp_sender_interface.cc
	#${webrtc_SOURCE_DIR}/api/rtp_sender_interface.h
	#${webrtc_SOURCE_DIR}/api/rtp_transceiver_direction.h
	#${webrtc_SOURCE_DIR}/api/rtp_transceiver_interface.cc
	#${webrtc_SOURCE_DIR}/api/rtp_transceiver_interface.h
	${webrtc_SOURCE_DIR}/api/scoped_refptr.h
	${webrtc_SOURCE_DIR}/api/task_queue/default_task_queue_factory.h
	${webrtc_SOURCE_DIR}/api/task_queue/queued_task.h
	${webrtc_SOURCE_DIR}/api/task_queue/task_queue_base.cc
	${webrtc_SOURCE_DIR}/api/task_queue/task_queue_base.h
	${webrtc_SOURCE_DIR}/api/task_queue/task_queue_factory.h
	${webrtc_SOURCE_DIR}/api/transport/bitrate_settings.cc
	${webrtc_SOURCE_DIR}/api/transport/bitrate_settings.h
	${webrtc_SOURCE_DIR}/api/transport/data_channel_transport_interface.h
	${webrtc_SOURCE_DIR}/api/transport/enums.h
	${webrtc_SOURCE_DIR}/api/transport/network_control.h
	${webrtc_SOURCE_DIR}/api/transport/network_types.cc
	${webrtc_SOURCE_DIR}/api/transport/network_types.h
	${webrtc_SOURCE_DIR}/api/transport/rtp/dependency_descriptor.cc
	${webrtc_SOURCE_DIR}/api/transport/rtp/dependency_descriptor.h
	${webrtc_SOURCE_DIR}/api/transport/rtp/rtp_source.h
	${webrtc_SOURCE_DIR}/api/transport/stun.cc
	${webrtc_SOURCE_DIR}/api/transport/stun.h
	${webrtc_SOURCE_DIR}/api/transport/webrtc_key_value_config.h
	${webrtc_SOURCE_DIR}/api/turn_customizer.h
	${webrtc_SOURCE_DIR}/api/units/data_rate.cc
	${webrtc_SOURCE_DIR}/api/units/data_rate.h
	${webrtc_SOURCE_DIR}/api/units/data_size.cc
	${webrtc_SOURCE_DIR}/api/units/data_size.h
	${webrtc_SOURCE_DIR}/api/units/frequency.cc
	${webrtc_SOURCE_DIR}/api/units/frequency.h
	${webrtc_SOURCE_DIR}/api/units/time_delta.cc
	${webrtc_SOURCE_DIR}/api/units/time_delta.h
	${webrtc_SOURCE_DIR}/api/units/timestamp.cc
	${webrtc_SOURCE_DIR}/api/units/timestamp.h
	#${webrtc_SOURCE_DIR}/api/video_codecs/sdp_video_format.cc
	#${webrtc_SOURCE_DIR}/api/video_codecs/sdp_video_format.h
	#${webrtc_SOURCE_DIR}/api/video_codecs/video_codec.cc
	#${webrtc_SOURCE_DIR}/api/video_codecs/video_codec.h
	#${webrtc_SOURCE_DIR}/api/video_codecs/video_decoder.cc
	#${webrtc_SOURCE_DIR}/api/video_codecs/video_decoder.h
	#${webrtc_SOURCE_DIR}/api/video_codecs/video_encoder_config.cc
	#${webrtc_SOURCE_DIR}/api/video_codecs/video_encoder_config.h
	#${webrtc_SOURCE_DIR}/api/video_codecs/video_encoder_factory.h
	#${webrtc_SOURCE_DIR}/api/video_codecs/video_encoder.cc
	#${webrtc_SOURCE_DIR}/api/video_codecs/video_encoder.h
	#${webrtc_SOURCE_DIR}/api/video/color_space.cc
	#${webrtc_SOURCE_DIR}/api/video/color_space.h
	#${webrtc_SOURCE_DIR}/api/video/encoded_frame.cc
	#${webrtc_SOURCE_DIR}/api/video/encoded_frame.h
	#${webrtc_SOURCE_DIR}/api/video/encoded_image.cc
	#${webrtc_SOURCE_DIR}/api/video/encoded_image.h
	#${webrtc_SOURCE_DIR}/api/video/hdr_metadata.cc
	#${webrtc_SOURCE_DIR}/api/video/hdr_metadata.h
	#${webrtc_SOURCE_DIR}/api/video/recordable_encoded_frame.h
	#${webrtc_SOURCE_DIR}/api/video/video_bitrate_allocation.cc
	#${webrtc_SOURCE_DIR}/api/video/video_bitrate_allocation.h
	#${webrtc_SOURCE_DIR}/api/video/video_bitrate_allocator_factory.h
	#${webrtc_SOURCE_DIR}/api/video/video_bitrate_allocator.cc
	#${webrtc_SOURCE_DIR}/api/video/video_bitrate_allocator.h
	#${webrtc_SOURCE_DIR}/api/video/video_codec_constants.h
	#${webrtc_SOURCE_DIR}/api/video/video_codec_type.h
	#${webrtc_SOURCE_DIR}/api/video/video_content_type.cc
	#${webrtc_SOURCE_DIR}/api/video/video_content_type.h
	#${webrtc_SOURCE_DIR}/api/video/video_frame_buffer.cc
	#${webrtc_SOURCE_DIR}/api/video/video_frame_buffer.h
	#${webrtc_SOURCE_DIR}/api/video/video_frame_metadata.cc
	#${webrtc_SOURCE_DIR}/api/video/video_frame_metadata.h
	#${webrtc_SOURCE_DIR}/api/video/video_frame_type.h
	#${webrtc_SOURCE_DIR}/api/video/video_frame.cc
	#${webrtc_SOURCE_DIR}/api/video/video_frame.h
	#${webrtc_SOURCE_DIR}/api/video/video_rotation.h
	#${webrtc_SOURCE_DIR}/api/video/video_sink_interface.h
	#${webrtc_SOURCE_DIR}/api/video/video_source_interface.cc
	#${webrtc_SOURCE_DIR}/api/video/video_source_interface.h
	#${webrtc_SOURCE_DIR}/api/video/video_stream_encoder_settings.h
	#${webrtc_SOURCE_DIR}/api/video/video_timing.cc
	#${webrtc_SOURCE_DIR}/api/video/video_timing.h
	#${webrtc_SOURCE_DIR}/call/audio_receive_stream.cc
	#${webrtc_SOURCE_DIR}/call/audio_receive_stream.h
	#${webrtc_SOURCE_DIR}/call/audio_send_stream.cc
	#${webrtc_SOURCE_DIR}/call/audio_send_stream.h
	#${webrtc_SOURCE_DIR}/call/audio_sender.h
	#${webrtc_SOURCE_DIR}/call/audio_state.cc
	#${webrtc_SOURCE_DIR}/call/audio_state.h
	#${webrtc_SOURCE_DIR}/call/call_config.cc
	#${webrtc_SOURCE_DIR}/call/call_config.h
	#${webrtc_SOURCE_DIR}/call/call.cc
	#${webrtc_SOURCE_DIR}/call/call.h
	#${webrtc_SOURCE_DIR}/call/flexfec_receive_stream.cc
	#${webrtc_SOURCE_DIR}/call/flexfec_receive_stream.h
	#${webrtc_SOURCE_DIR}/call/packet_receiver.h
	#${webrtc_SOURCE_DIR}/call/rtp_config.cc
	#${webrtc_SOURCE_DIR}/call/rtp_config.h
	#${webrtc_SOURCE_DIR}/call/rtp_demuxer.cc
	#${webrtc_SOURCE_DIR}/call/rtp_demuxer.h
	#${webrtc_SOURCE_DIR}/call/rtp_packet_sink_interface.h
	#${webrtc_SOURCE_DIR}/call/rtp_rtcp_demuxer_helper.cc
	#${webrtc_SOURCE_DIR}/call/rtp_rtcp_demuxer_helper.h
	#${webrtc_SOURCE_DIR}/call/rtp_transport_controller_send_interface.h
	#${webrtc_SOURCE_DIR}/call/ssrc_binding_observer.h
	#${webrtc_SOURCE_DIR}/call/video_receive_stream.cc
	#${webrtc_SOURCE_DIR}/call/video_receive_stream.h
	#${webrtc_SOURCE_DIR}/call/video_send_stream.cc
	#${webrtc_SOURCE_DIR}/call/video_send_stream.h
	${webrtc_SOURCE_DIR}/common_types.h
	#${webrtc_SOURCE_DIR}/common_video/generic_frame_descriptor/generic_frame_info.cc
	#${webrtc_SOURCE_DIR}/common_video/generic_frame_descriptor/generic_frame_info.h
	#${webrtc_SOURCE_DIR}/common_video/include/quality_limitation_reason.h
	${webrtc_SOURCE_DIR}/logging/rtc_event_log/events/rtc_event_dtls_transport_state.cc
	${webrtc_SOURCE_DIR}/logging/rtc_event_log/events/rtc_event_dtls_transport_state.h
	${webrtc_SOURCE_DIR}/logging/rtc_event_log/events/rtc_event_dtls_writable_state.cc
	${webrtc_SOURCE_DIR}/logging/rtc_event_log/events/rtc_event_dtls_writable_state.h
	${webrtc_SOURCE_DIR}/logging/rtc_event_log/events/rtc_event_ice_candidate_pair_config.cc
	${webrtc_SOURCE_DIR}/logging/rtc_event_log/events/rtc_event_ice_candidate_pair_config.h
	${webrtc_SOURCE_DIR}/logging/rtc_event_log/events/rtc_event_ice_candidate_pair.cc
	${webrtc_SOURCE_DIR}/logging/rtc_event_log/events/rtc_event_ice_candidate_pair.h
	${webrtc_SOURCE_DIR}/logging/rtc_event_log/ice_logger.cc
	${webrtc_SOURCE_DIR}/logging/rtc_event_log/ice_logger.h
	#${webrtc_SOURCE_DIR}/media/base/codec.cc
	#${webrtc_SOURCE_DIR}/media/base/codec.h
	#${webrtc_SOURCE_DIR}/media/base/delayable.h
	#${webrtc_SOURCE_DIR}/media/base/h264_profile_level_id.cc
	#${webrtc_SOURCE_DIR}/media/base/h264_profile_level_id.h
	#${webrtc_SOURCE_DIR}/media/base/media_channel.cc
	#${webrtc_SOURCE_DIR}/media/base/media_channel.h
	#${webrtc_SOURCE_DIR}/media/base/media_config.h
	#${webrtc_SOURCE_DIR}/media/base/media_constants.cc
	#${webrtc_SOURCE_DIR}/media/base/media_constants.h
	#${webrtc_SOURCE_DIR}/media/base/media_engine.cc
	#${webrtc_SOURCE_DIR}/media/base/media_engine.h
	#${webrtc_SOURCE_DIR}/media/base/rid_description.cc
	#${webrtc_SOURCE_DIR}/media/base/rid_description.cc
	#${webrtc_SOURCE_DIR}/media/base/rid_description.h
	#${webrtc_SOURCE_DIR}/media/base/rtp_utils.cc
	#${webrtc_SOURCE_DIR}/media/base/rtp_utils.h
	#${webrtc_SOURCE_DIR}/media/base/stream_params.cc
	#${webrtc_SOURCE_DIR}/media/base/stream_params.h
	#${webrtc_SOURCE_DIR}/media/base/turn_utils.cc
	#${webrtc_SOURCE_DIR}/media/base/turn_utils.cc
	#${webrtc_SOURCE_DIR}/media/base/turn_utils.h
	#${webrtc_SOURCE_DIR}/media/base/video_common.cc
	#${webrtc_SOURCE_DIR}/media/base/video_common.h
	#${webrtc_SOURCE_DIR}/media/base/vp9_profile.cc
	#${webrtc_SOURCE_DIR}/media/base/vp9_profile.h
	#${webrtc_SOURCE_DIR}/modules/audio_device/include/audio_device_defines.h
	#${webrtc_SOURCE_DIR}/modules/audio_device/include/audio_device.h
	#${webrtc_SOURCE_DIR}/modules/audio_processing/include/audio_processing_statistics.cc
	#${webrtc_SOURCE_DIR}/modules/audio_processing/include/audio_processing_statistics.h
	#${webrtc_SOURCE_DIR}/modules/audio_processing/include/audio_processing.cc
	#${webrtc_SOURCE_DIR}/modules/audio_processing/include/audio_processing.h
	#${webrtc_SOURCE_DIR}/modules/audio_processing/include/config.cc
	#${webrtc_SOURCE_DIR}/modules/audio_processing/include/config.h
	#${webrtc_SOURCE_DIR}/modules/include/module_common_types.cc
	#${webrtc_SOURCE_DIR}/modules/include/module_common_types.h
	#${webrtc_SOURCE_DIR}/modules/include/module_fec_types.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/include/report_block_data.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/include/report_block_data.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/include/rtcp_statistics.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/include/rtp_header_extension_map.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/include/rtp_packet_sender.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/include/rtp_rtcp_defines.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/include/rtp_rtcp_defines.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/byte_io.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtcp_packet.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtcp_packet.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtcp_packet/app.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtcp_packet/app.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtcp_packet/common_header.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtcp_packet/common_header.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtcp_packet/remote_estimate.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtcp_packet/remote_estimate.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_dependency_descriptor_writer.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_dependency_descriptor_reader.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_dependency_descriptor_extension.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_dependency_descriptor_extension.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_generic_frame_descriptor_extension.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_generic_frame_descriptor_extension.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_generic_frame_descriptor.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_generic_frame_descriptor.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_header_extension_map.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_header_extensions.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_header_extensions.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_packet_received.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_packet_received.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_packet_to_send.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_packet_to_send.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_packet.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_packet.h
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_video_header.cc
	#${webrtc_SOURCE_DIR}/modules/rtp_rtcp/source/rtp_video_header.h
	${webrtc_SOURCE_DIR}/modules/utility/include/process_thread.h
	#${webrtc_SOURCE_DIR}/modules/video_coding/codecs/h264/include/h264_globals.h
	#${webrtc_SOURCE_DIR}/modules/video_coding/codecs/interface/common_constants.h
	#${webrtc_SOURCE_DIR}/modules/video_coding/codecs/vp8/include/vp8_globals.h
	#${webrtc_SOURCE_DIR}/modules/video_coding/codecs/vp9/include/vp9_globals.h
	#${webrtc_SOURCE_DIR}/modules/video_coding/encoded_frame.cc
	#${webrtc_SOURCE_DIR}/modules/video_coding/encoded_frame.h
	#${webrtc_SOURCE_DIR}/modules/video_coding/include/video_codec_interface.cc
	#${webrtc_SOURCE_DIR}/modules/video_coding/include/video_codec_interface.h
	#${webrtc_SOURCE_DIR}/modules/video_coding/include/video_coding_defines.h
	#${webrtc_SOURCE_DIR}/modules/video_coding/include/video_error_codes.h
	${webrtc_SOURCE_DIR}/p2p/base/async_stun_tcp_socket.cc
	${webrtc_SOURCE_DIR}/p2p/base/async_stun_tcp_socket.h
	${webrtc_SOURCE_DIR}/p2p/base/basic_ice_controller.cc
	${webrtc_SOURCE_DIR}/p2p/base/basic_ice_controller.h
	${webrtc_SOURCE_DIR}/p2p/base/basic_packet_socket_factory.cc
	${webrtc_SOURCE_DIR}/p2p/base/basic_packet_socket_factory.cc
	${webrtc_SOURCE_DIR}/p2p/base/basic_packet_socket_factory.h
	${webrtc_SOURCE_DIR}/p2p/base/candidate_pair_interface.h
	${webrtc_SOURCE_DIR}/p2p/base/connection_info.cc
	${webrtc_SOURCE_DIR}/p2p/base/connection_info.h
	${webrtc_SOURCE_DIR}/p2p/base/connection.cc
	${webrtc_SOURCE_DIR}/p2p/base/connection.h
	${webrtc_SOURCE_DIR}/p2p/base/dtls_transport_internal.cc
	${webrtc_SOURCE_DIR}/p2p/base/dtls_transport_internal.h
	${webrtc_SOURCE_DIR}/p2p/base/dtls_transport.cc
	${webrtc_SOURCE_DIR}/p2p/base/dtls_transport.h
	${webrtc_SOURCE_DIR}/p2p/base/ice_controller_factory_interface.h
	${webrtc_SOURCE_DIR}/p2p/base/ice_controller_interface.cc
	${webrtc_SOURCE_DIR}/p2p/base/ice_controller_interface.h
	${webrtc_SOURCE_DIR}/p2p/base/ice_credentials_iterator.cc
	${webrtc_SOURCE_DIR}/p2p/base/ice_credentials_iterator.h
	${webrtc_SOURCE_DIR}/p2p/base/ice_transport_internal.cc
	${webrtc_SOURCE_DIR}/p2p/base/ice_transport_internal.h
	${webrtc_SOURCE_DIR}/p2p/base/p2p_constants.cc
	${webrtc_SOURCE_DIR}/p2p/base/p2p_constants.h
	${webrtc_SOURCE_DIR}/p2p/base/p2p_transport_channel_ice_field_trials.h
	${webrtc_SOURCE_DIR}/p2p/base/p2p_transport_channel.cc
	${webrtc_SOURCE_DIR}/p2p/base/p2p_transport_channel.cc
	${webrtc_SOURCE_DIR}/p2p/base/p2p_transport_channel.h
	${webrtc_SOURCE_DIR}/p2p/base/packet_transport_internal.cc
	${webrtc_SOURCE_DIR}/p2p/base/packet_transport_internal.h
	${webrtc_SOURCE_DIR}/p2p/base/port_allocator.cc
	${webrtc_SOURCE_DIR}/p2p/base/port_allocator.h
	${webrtc_SOURCE_DIR}/p2p/base/port_interface.cc
	${webrtc_SOURCE_DIR}/p2p/base/port_interface.h
	${webrtc_SOURCE_DIR}/p2p/base/port.cc
	${webrtc_SOURCE_DIR}/p2p/base/port.h
	${webrtc_SOURCE_DIR}/p2p/base/regathering_controller.cc
	${webrtc_SOURCE_DIR}/p2p/base/regathering_controller.h
	${webrtc_SOURCE_DIR}/p2p/base/stun_port.cc
	${webrtc_SOURCE_DIR}/p2p/base/stun_port.h
	${webrtc_SOURCE_DIR}/p2p/base/stun_request.cc
	${webrtc_SOURCE_DIR}/p2p/base/stun_request.h
	${webrtc_SOURCE_DIR}/p2p/base/tcp_port.cc
	${webrtc_SOURCE_DIR}/p2p/base/tcp_port.h
	${webrtc_SOURCE_DIR}/p2p/base/transport_description_factory.cc
	${webrtc_SOURCE_DIR}/p2p/base/transport_description_factory.h
	${webrtc_SOURCE_DIR}/p2p/base/transport_description.cc
	${webrtc_SOURCE_DIR}/p2p/base/transport_description.h
	${webrtc_SOURCE_DIR}/p2p/base/transport_info.h
	${webrtc_SOURCE_DIR}/p2p/base/turn_port.cc
	${webrtc_SOURCE_DIR}/p2p/base/turn_port.h
	${webrtc_SOURCE_DIR}/p2p/base/udp_port.h
	${webrtc_SOURCE_DIR}/p2p/client/basic_port_allocator.cc
	${webrtc_SOURCE_DIR}/p2p/client/basic_port_allocator.cc
	${webrtc_SOURCE_DIR}/p2p/client/basic_port_allocator.h
	${webrtc_SOURCE_DIR}/p2p/client/relay_port_factory_interface.h
	${webrtc_SOURCE_DIR}/p2p/client/turn_port_factory.cc
	${webrtc_SOURCE_DIR}/p2p/client/turn_port_factory.h
	${webrtc_SOURCE_DIR}/pc/channel_interface.h
	${webrtc_SOURCE_DIR}/pc/channel_manager.cc
	${webrtc_SOURCE_DIR}/pc/channel_manager.cc
	${webrtc_SOURCE_DIR}/pc/channel_manager.h
	${webrtc_SOURCE_DIR}/pc/channel.cc
	${webrtc_SOURCE_DIR}/pc/channel.h
	#${webrtc_SOURCE_DIR}/pc/composite_data_channel_transport.cc
	#${webrtc_SOURCE_DIR}/pc/composite_data_channel_transport.h
	#${webrtc_SOURCE_DIR}/pc/composite_rtp_transport.cc
	#${webrtc_SOURCE_DIR}/pc/composite_rtp_transport.h
	${webrtc_SOURCE_DIR}/pc/dtls_transport.cc
	${webrtc_SOURCE_DIR}/pc/dtls_transport.h
	${webrtc_SOURCE_DIR}/pc/ice_transport.cc
	${webrtc_SOURCE_DIR}/pc/ice_transport.h
	#${webrtc_SOURCE_DIR}/pc/jsep_ice_candidate.cc
	#${webrtc_SOURCE_DIR}/pc/jsep_session_description.cc
	#${webrtc_SOURCE_DIR}/pc/jsep_transport.cc
	#${webrtc_SOURCE_DIR}/pc/jsep_transport.h
	#${webrtc_SOURCE_DIR}/pc/media_protocol_names.cc
	#${webrtc_SOURCE_DIR}/pc/media_protocol_names.h
	#${webrtc_SOURCE_DIR}/pc/media_session.cc
	#${webrtc_SOURCE_DIR}/pc/media_session.h
	#${webrtc_SOURCE_DIR}/pc/rtp_media_utils.cc
	#${webrtc_SOURCE_DIR}/pc/rtp_media_utils.h
	#${webrtc_SOURCE_DIR}/pc/rtp_transport_internal.h
	#${webrtc_SOURCE_DIR}/pc/rtp_transport.cc
	#${webrtc_SOURCE_DIR}/pc/rtp_transport.h
	#${webrtc_SOURCE_DIR}/pc/sctp_data_channel_transport.cc
	#${webrtc_SOURCE_DIR}/pc/sctp_data_channel_transport.h
	${webrtc_SOURCE_DIR}/pc/sdp_serializer.cc
	${webrtc_SOURCE_DIR}/pc/sdp_serializer.h
	${webrtc_SOURCE_DIR}/pc/session_description.cc
	${webrtc_SOURCE_DIR}/pc/session_description.h
	${webrtc_SOURCE_DIR}/pc/simulcast_description.cc
	${webrtc_SOURCE_DIR}/pc/simulcast_description.h
	${webrtc_SOURCE_DIR}/pc/transport_stats.cc
	${webrtc_SOURCE_DIR}/pc/transport_stats.h
	${webrtc_SOURCE_DIR}/pc/used_ids.h
	#${webrtc_SOURCE_DIR}/pc/webrtc_sdp.cc
	webrtc_sdp.cc  # NOTE: This is a file that we had to hack.  See the comments at the top of the file for more info.
	${webrtc_SOURCE_DIR}/pc/webrtc_sdp.h
	${webrtc_SOURCE_DIR}/rtc_base/arraysize.h
	${webrtc_SOURCE_DIR}/rtc_base/async_invoker_inl.h
	${webrtc_SOURCE_DIR}/rtc_base/async_invoker.cc
	${webrtc_SOURCE_DIR}/rtc_base/async_invoker.h
	${webrtc_SOURCE_DIR}/rtc_base/async_packet_socket.cc
	${webrtc_SOURCE_DIR}/rtc_base/async_packet_socket.h
	${webrtc_SOURCE_DIR}/rtc_base/async_resolver_interface.cc
	${webrtc_SOURCE_DIR}/rtc_base/async_resolver_interface.h
	${webrtc_SOURCE_DIR}/rtc_base/async_socket.cc
	${webrtc_SOURCE_DIR}/rtc_base/async_socket.h
	${webrtc_SOURCE_DIR}/rtc_base/async_tcp_socket.cc
	${webrtc_SOURCE_DIR}/rtc_base/async_tcp_socket.h
	${webrtc_SOURCE_DIR}/rtc_base/async_udp_socket.cc
	${webrtc_SOURCE_DIR}/rtc_base/async_udp_socket.h
	${webrtc_SOURCE_DIR}/rtc_base/atomic_ops.h
	${webrtc_SOURCE_DIR}/rtc_base/bind.h
	${webrtc_SOURCE_DIR}/rtc_base/bit_buffer.cc
	${webrtc_SOURCE_DIR}/rtc_base/buffer_queue.cc
	${webrtc_SOURCE_DIR}/rtc_base/buffer_queue.h
	${webrtc_SOURCE_DIR}/rtc_base/buffer.h
	${webrtc_SOURCE_DIR}/rtc_base/byte_buffer.cc
	${webrtc_SOURCE_DIR}/rtc_base/byte_buffer.h
	${webrtc_SOURCE_DIR}/rtc_base/byte_order.h
	${webrtc_SOURCE_DIR}/rtc_base/callback.h
	${webrtc_SOURCE_DIR}/rtc_base/checks.cc
	${webrtc_SOURCE_DIR}/rtc_base/checks.h
	${webrtc_SOURCE_DIR}/rtc_base/constructor_magic.h
	${webrtc_SOURCE_DIR}/rtc_base/copy_on_write_buffer.cc
	${webrtc_SOURCE_DIR}/rtc_base/copy_on_write_buffer.h
	${webrtc_SOURCE_DIR}/rtc_base/crc32.cc
	${webrtc_SOURCE_DIR}/rtc_base/crc32.h
	${webrtc_SOURCE_DIR}/rtc_base/critical_section.cc
	${webrtc_SOURCE_DIR}/rtc_base/critical_section.h
	${webrtc_SOURCE_DIR}/rtc_base/crypt_string.cc
	${webrtc_SOURCE_DIR}/rtc_base/crypt_string.h
	${webrtc_SOURCE_DIR}/rtc_base/deprecated/signal_thread.cc
	${webrtc_SOURCE_DIR}/rtc_base/deprecated/signal_thread.h
	${webrtc_SOURCE_DIR}/rtc_base/deprecation.h
	${webrtc_SOURCE_DIR}/rtc_base/dscp.h
	${webrtc_SOURCE_DIR}/rtc_base/event_tracer.cc
	${webrtc_SOURCE_DIR}/rtc_base/event_tracer.cc
	${webrtc_SOURCE_DIR}/rtc_base/event_tracer.h
	${webrtc_SOURCE_DIR}/rtc_base/event.cc
	${webrtc_SOURCE_DIR}/rtc_base/event.h
	${webrtc_SOURCE_DIR}/rtc_base/experiments/field_trial_parser.cc
	${webrtc_SOURCE_DIR}/rtc_base/experiments/field_trial_parser.h
	${webrtc_SOURCE_DIR}/rtc_base/experiments/field_trial_units.cc
	${webrtc_SOURCE_DIR}/rtc_base/experiments/field_trial_units.h
	${webrtc_SOURCE_DIR}/rtc_base/experiments/struct_parameters_parser.cc
	${webrtc_SOURCE_DIR}/rtc_base/experiments/struct_parameters_parser.h
	${webrtc_SOURCE_DIR}/rtc_base/helpers.cc
	${webrtc_SOURCE_DIR}/rtc_base/helpers.h
	${webrtc_SOURCE_DIR}/rtc_base/http_common.cc
	${webrtc_SOURCE_DIR}/rtc_base/http_common.cc
	${webrtc_SOURCE_DIR}/rtc_base/http_common.h
	${webrtc_SOURCE_DIR}/rtc_base/ip_address.cc
	${webrtc_SOURCE_DIR}/rtc_base/ip_address.h
	${webrtc_SOURCE_DIR}/rtc_base/location.cc
	${webrtc_SOURCE_DIR}/rtc_base/location.h
	${webrtc_SOURCE_DIR}/rtc_base/logging.cc
	${webrtc_SOURCE_DIR}/rtc_base/logging.h
	${webrtc_SOURCE_DIR}/rtc_base/mdns_responder_interface.h
	${webrtc_SOURCE_DIR}/rtc_base/message_digest.cc
	${webrtc_SOURCE_DIR}/rtc_base/message_digest.h
	${webrtc_SOURCE_DIR}/rtc_base/message_handler.cc
	${webrtc_SOURCE_DIR}/rtc_base/message_handler.h
	${webrtc_SOURCE_DIR}/rtc_base/net_helper.cc
	${webrtc_SOURCE_DIR}/rtc_base/net_helper.h
	${webrtc_SOURCE_DIR}/rtc_base/net_helpers.cc
	${webrtc_SOURCE_DIR}/rtc_base/net_helpers.h
	${webrtc_SOURCE_DIR}/rtc_base/network_constants.cc
	${webrtc_SOURCE_DIR}/rtc_base/network_constants.h
	${webrtc_SOURCE_DIR}/rtc_base/network_monitor.cc
	${webrtc_SOURCE_DIR}/rtc_base/network_monitor.h
	${webrtc_SOURCE_DIR}/rtc_base/network_route.cc
	${webrtc_SOURCE_DIR}/rtc_base/network_route.h
	${webrtc_SOURCE_DIR}/rtc_base/network.cc
	${webrtc_SOURCE_DIR}/rtc_base/network.h
	${webrtc_SOURCE_DIR}/rtc_base/network/sent_packet.cc
	${webrtc_SOURCE_DIR}/rtc_base/network/sent_packet.h
	${webrtc_SOURCE_DIR}/rtc_base/null_socket_server.cc
	${webrtc_SOURCE_DIR}/rtc_base/null_socket_server.h
	${webrtc_SOURCE_DIR}/rtc_base/numerics/event_based_exponential_moving_average.cc
	${webrtc_SOURCE_DIR}/rtc_base/numerics/event_based_exponential_moving_average.h
	${webrtc_SOURCE_DIR}/rtc_base/numerics/safe_compare.h
	${webrtc_SOURCE_DIR}/rtc_base/numerics/safe_conversions_impl.h
	${webrtc_SOURCE_DIR}/rtc_base/numerics/safe_conversions.h
	${webrtc_SOURCE_DIR}/rtc_base/numerics/safe_minmax.h
	${webrtc_SOURCE_DIR}/rtc_base/openssl_adapter.cc
	${webrtc_SOURCE_DIR}/rtc_base/openssl_adapter.h
	${webrtc_SOURCE_DIR}/rtc_base/openssl_certificate.cc
	${webrtc_SOURCE_DIR}/rtc_base/openssl_certificate.h
	${webrtc_SOURCE_DIR}/rtc_base/openssl_digest.cc
	${webrtc_SOURCE_DIR}/rtc_base/openssl_digest.h
	${webrtc_SOURCE_DIR}/rtc_base/openssl_identity.cc
	${webrtc_SOURCE_DIR}/rtc_base/openssl_identity.h
	${webrtc_SOURCE_DIR}/rtc_base/openssl_session_cache.cc
	${webrtc_SOURCE_DIR}/rtc_base/openssl_session_cache.h
	${webrtc_SOURCE_DIR}/rtc_base/openssl_stream_adapter.cc
	${webrtc_SOURCE_DIR}/rtc_base/openssl_stream_adapter.h
	${webrtc_SOURCE_DIR}/rtc_base/openssl_utility.cc
	${webrtc_SOURCE_DIR}/rtc_base/openssl_utility.h
	${webrtc_SOURCE_DIR}/rtc_base/openssl.h
	${webrtc_SOURCE_DIR}/rtc_base/physical_socket_server.cc
	${webrtc_SOURCE_DIR}/rtc_base/physical_socket_server.h
	${webrtc_SOURCE_DIR}/rtc_base/platform_thread_types.cc
	${webrtc_SOURCE_DIR}/rtc_base/platform_thread_types.h
	${webrtc_SOURCE_DIR}/rtc_base/platform_thread.cc
	${webrtc_SOURCE_DIR}/rtc_base/platform_thread.cc
	${webrtc_SOURCE_DIR}/rtc_base/platform_thread.h
	${webrtc_SOURCE_DIR}/rtc_base/proxy_info.cc
	${webrtc_SOURCE_DIR}/rtc_base/proxy_info.h
	${webrtc_SOURCE_DIR}/rtc_base/rate_tracker.cc
	${webrtc_SOURCE_DIR}/rtc_base/rate_tracker.h
	${webrtc_SOURCE_DIR}/rtc_base/ref_count.h
	${webrtc_SOURCE_DIR}/rtc_base/ref_counted_object.h
	${webrtc_SOURCE_DIR}/rtc_base/ref_counter.h
	${webrtc_SOURCE_DIR}/rtc_base/rtc_certificate.cc
	${webrtc_SOURCE_DIR}/rtc_base/rtc_certificate.h
	${webrtc_SOURCE_DIR}/rtc_base/sanitizer.h
	${webrtc_SOURCE_DIR}/rtc_base/signal_thread.h
	${webrtc_SOURCE_DIR}/rtc_base/socket_adapters.cc
	${webrtc_SOURCE_DIR}/rtc_base/socket_adapters.h
	${webrtc_SOURCE_DIR}/rtc_base/socket_address.cc
	${webrtc_SOURCE_DIR}/rtc_base/socket_address.h
	${webrtc_SOURCE_DIR}/rtc_base/socket_factory.h
	${webrtc_SOURCE_DIR}/rtc_base/socket_server.h
	${webrtc_SOURCE_DIR}/rtc_base/socket.cc
	${webrtc_SOURCE_DIR}/rtc_base/socket.h
	${webrtc_SOURCE_DIR}/rtc_base/ssl_adapter.cc
	${webrtc_SOURCE_DIR}/rtc_base/ssl_adapter.h
	${webrtc_SOURCE_DIR}/rtc_base/ssl_certificate.cc
	${webrtc_SOURCE_DIR}/rtc_base/ssl_certificate.h
	${webrtc_SOURCE_DIR}/rtc_base/ssl_fingerprint.cc
	${webrtc_SOURCE_DIR}/rtc_base/ssl_fingerprint.h
	${webrtc_SOURCE_DIR}/rtc_base/ssl_identity.cc
	${webrtc_SOURCE_DIR}/rtc_base/ssl_identity.h
	${webrtc_SOURCE_DIR}/rtc_base/ssl_roots.h
	${webrtc_SOURCE_DIR}/rtc_base/ssl_stream_adapter.cc
	${webrtc_SOURCE_DIR}/rtc_base/ssl_stream_adapter.h
	${webrtc_SOURCE_DIR}/rtc_base/stream.cc
	${webrtc_SOURCE_DIR}/rtc_base/stream.h
	${webrtc_SOURCE_DIR}/rtc_base/string_encode.cc
	${webrtc_SOURCE_DIR}/rtc_base/string_encode.h
	${webrtc_SOURCE_DIR}/rtc_base/string_to_number.cc
	${webrtc_SOURCE_DIR}/rtc_base/string_to_number.h
	${webrtc_SOURCE_DIR}/rtc_base/string_utils.cc
	${webrtc_SOURCE_DIR}/rtc_base/string_utils.h
	${webrtc_SOURCE_DIR}/rtc_base/stringize_macros.h
	${webrtc_SOURCE_DIR}/rtc_base/strings/string_builder.cc
	${webrtc_SOURCE_DIR}/rtc_base/strings/string_builder.h
	${webrtc_SOURCE_DIR}/rtc_base/synchronization/mutex_critical_section.h
	${webrtc_SOURCE_DIR}/rtc_base/synchronization/mutex.cc
	${webrtc_SOURCE_DIR}/rtc_base/synchronization/mutex.h
	${webrtc_SOURCE_DIR}/rtc_base/synchronization/rw_lock_wrapper.cc
	${webrtc_SOURCE_DIR}/rtc_base/synchronization/rw_lock_wrapper.h
	${webrtc_SOURCE_DIR}/rtc_base/synchronization/sequence_checker.cc
	${webrtc_SOURCE_DIR}/rtc_base/synchronization/sequence_checker.h
	${webrtc_SOURCE_DIR}/rtc_base/synchronization/yield_policy.cc
	${webrtc_SOURCE_DIR}/rtc_base/synchronization/yield_policy.cc
	${webrtc_SOURCE_DIR}/rtc_base/synchronization/yield_policy.h
	${webrtc_SOURCE_DIR}/rtc_base/synchronization/yield.cc
	${webrtc_SOURCE_DIR}/rtc_base/synchronization/yield.h
	${webrtc_SOURCE_DIR}/rtc_base/system/arch.h
	${webrtc_SOURCE_DIR}/rtc_base/system/file_wrapper.cc
	${webrtc_SOURCE_DIR}/rtc_base/system/file_wrapper.h
	${webrtc_SOURCE_DIR}/rtc_base/system/inline.h
	${webrtc_SOURCE_DIR}/rtc_base/system/rtc_export.h
	${webrtc_SOURCE_DIR}/rtc_base/system/unused.h
	${webrtc_SOURCE_DIR}/rtc_base/system/warn_current_thread_is_deadlocked.h
#	${webrtc_SOURCE_DIR}/rtc_base/task_queue.cc
#	${webrtc_SOURCE_DIR}/rtc_base/task_queue.h
	${webrtc_SOURCE_DIR}/rtc_base/task_utils/pending_task_safety_flag.cc
	${webrtc_SOURCE_DIR}/rtc_base/task_utils/pending_task_safety_flag.h
	${webrtc_SOURCE_DIR}/rtc_base/task_utils/to_queued_task.h
	${webrtc_SOURCE_DIR}/rtc_base/third_party/base64/base64.cc
	${webrtc_SOURCE_DIR}/rtc_base/third_party/base64/base64.h
	${webrtc_SOURCE_DIR}/rtc_base/third_party/sigslot/sigslot.cc
	${webrtc_SOURCE_DIR}/rtc_base/third_party/sigslot/sigslot.h
	${webrtc_SOURCE_DIR}/rtc_base/thread_annotations.h
	${webrtc_SOURCE_DIR}/rtc_base/thread_checker.h
	${webrtc_SOURCE_DIR}/rtc_base/thread_message.h
	${webrtc_SOURCE_DIR}/rtc_base/thread.cc
	${webrtc_SOURCE_DIR}/rtc_base/thread.h
	${webrtc_SOURCE_DIR}/rtc_base/time_utils.cc
	${webrtc_SOURCE_DIR}/rtc_base/time_utils.h
	${webrtc_SOURCE_DIR}/rtc_base/trace_event.h
	${webrtc_SOURCE_DIR}/rtc_base/type_traits.h
	${webrtc_SOURCE_DIR}/rtc_base/unique_id_generator.cc
	${webrtc_SOURCE_DIR}/rtc_base/unique_id_generator.h
	${webrtc_SOURCE_DIR}/rtc_base/units/unit_base.h
	${webrtc_SOURCE_DIR}/rtc_base/weak_ptr.cc
	${webrtc_SOURCE_DIR}/rtc_base/weak_ptr.h
	${webrtc_SOURCE_DIR}/rtc_base/zero_memory.cc
	${webrtc_SOURCE_DIR}/rtc_base/zero_memory.h
	${webrtc_SOURCE_DIR}/system_wrappers/include/clock.h
	${webrtc_SOURCE_DIR}/system_wrappers/include/field_trial.h
	${webrtc_SOURCE_DIR}/system_wrappers/include/metrics.h
	${webrtc_SOURCE_DIR}/system_wrappers/include/ntp_time.h
	${webrtc_SOURCE_DIR}/system_wrappers/source/clock.cc
	${webrtc_SOURCE_DIR}/system_wrappers/source/field_trial.cc
	${webrtc_SOURCE_DIR}/system_wrappers/source/metrics.cc
	)

if(WIN32)
	target_sources(webrtc-lite PRIVATE
		${webrtc_SOURCE_DIR}/rtc_base/win32.cc
		${webrtc_SOURCE_DIR}/rtc_base/win32.h
		${webrtc_SOURCE_DIR}/rtc_base/synchronization/rw_lock_win.cc
		${webrtc_SOURCE_DIR}/rtc_base/synchronization/rw_lock_win.h
		)
	FILE(WRITE ${webrtc_SOURCE_DIR}/base/third_party/libevent/event.h
		"#pragma once\n#include <event.h>\n"
		)
	target_link_libraries(webrtc-lite
		ws2_32
		crypt32
		winmm
		Secur32
		Iphlpapi
		)
else(WIN32)
	target_sources(webrtc-lite PRIVATE
		${webrtc_SOURCE_DIR}/rtc_base/synchronization/rw_lock_posix.cc
		${webrtc_SOURCE_DIR}/rtc_base/synchronization/rw_lock_posix.h
		${webrtc_SOURCE_DIR}/rtc_base/ifaddrs_converter.cc
		)
	target_link_libraries(webrtc-lite
		pthread
		)
	target_compile_options(webrtc-lite PRIVATE -Wno-attributes)
endif(WIN32)

function( compile_webrtc_proto SRCPROTO OUTDIR )
	get_filename_component(ABS_FIL ${SRCPROTO} ABSOLUTE)
	get_filename_component(ABS_PATH ${ABS_FIL} PATH)
	get_filename_component(FIL_WE ${SRCPROTO} NAME_WE)
	set(OUT_CC "${OUTDIR}/${FIL_WE}.pb.cc")
	set(OUT_H "${OUTDIR}/${FIL_WE}.pb.h")
	add_custom_command(
		OUTPUT ${OUT_CC} ${OUT_H}
		COMMAND ${PROTOBUF_PROTOC_EXECUTABLE}
		ARGS --cpp_out ${OUTDIR} -I ${ABS_PATH} ${ABS_FIL}
		DEPENDS ${SRCPROTO} ${PROTOBUF_PROTOC_EXECUTABLE}
		COMMENT "Running C++ protocol buffer compiler on ${SRCPROTO}"
		VERBATIM )
	target_sources(webrtc-lite PRIVATE ${OUT_CC} ${OUT_H})
	set_source_files_properties(${OUT_CC} ${OUT_H} PROPERTIES GENERATED TRUE)
endfunction( compile_webrtc_proto )

# webrtc can't build as shared library.
#target_compile_definitions(webrtc-lite PUBLIC WEBRTC_ENABLE_SYMBOL_EXPORT)
#target_compile_definitions(webrtc-lite PRIVATE WEBRTC_LIBRARY_IMPL)

# FIXME - not supported on older cmake
#target_link_directories(webrtc-lite PUBLIC
#    ${CONAN_ABSEIL_ROOT}/lib
#)

target_include_directories(webrtc-lite PRIVATE
	${webrtc_SOURCE_DIR}
	#${webrtc_BUILD_DIR}  # ???
	${CMAKE_CURRENT_BINARY_DIR}/..
	${CMAKE_CURRENT_BINARY_DIR}
	)

set(ABSEIL_DEPENDENCIES 
	absl::meta
	absl::optional
	absl::strings
	)

if(LTO)
	foreach(TGT IN LISTS ABSEIL_DEPENDENCIES)
		set_target_common_gns_properties(${TGT})
	endforeach()
endif()

target_link_libraries(webrtc-lite
	OpenSSL::Crypto
	OpenSSL::SSL
	${ABSEIL_DEPENDENCIES}
	)

set_target_common_gns_properties(webrtc-lite)

set_target_properties(webrtc-lite PROPERTIES
	CXX_STANDARD 14
	)

set_property(TARGET webrtc-lite PROPERTY POSITION_INDEPENDENT_CODE ON)

add_library(steamwebrtc STATIC
	"ice_session.h"
	"steamwebrtc_internal.h"
	"ice_session.cpp"
	)

target_include_directories(steamwebrtc PRIVATE
	${webrtc_SOURCE_DIR}/include/webrtc
	${webrtc_SOURCE_DIR}
	)

set_target_common_gns_properties(steamwebrtc)

target_link_libraries(steamwebrtc
	webrtc-lite
	)

set_target_properties(steamwebrtc PROPERTIES
	CXX_STANDARD 14
	)

set_property(TARGET steamwebrtc PROPERTY POSITION_INDEPENDENT_CODE ON)

install(
	TARGETS
		steamwebrtc webrtc-lite
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	)
