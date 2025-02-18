// ConsoleApplication2.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <librealsense2/rs.hpp> // Include RealSense Cross Platform API
#include "../example.hpp"          // Include short list of convenience functions for rendering
#include <librealsense2/rs_advanced_mode.hpp>

#define _USE_MATH_DEFINES
#include <math.h>
#include <queue>
#include <unordered_set>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <streambuf>
#include <fstream>
#include <sstream>              // Stringstreams
#include <iostream>             // Terminal IO
#include <chrono>				// timestap file naming

// stb library
#define STBI_MSC_SECURE_CRT
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../stb_image_write_109.h" //v1.09 nothing_stb
// opencv for imwrite
#include <opencv2/opencv.hpp>


// Helper function for writing metadata to disk as a csv file
void metadata_to_csv(const rs2::frame& frm, const std::string& filename);
void capture_light1(const rs2::frameset& frmset, const rs2::frameset& rawset,const std::chrono::seconds&, const char& workKey);


rs2::frameset worldFrameset;			// to be saved video 8bit Frameset
rs2::frameset rawFrameset;				// 16 bit grayscale depth fs

// colorizer scale
const float cz_minZ = 0.2f;
const float cz_maxZ = 0.4f;

int main(int argc, char * argv[]) try
{
	rs2::log_to_console(RS2_LOG_SEVERITY_ERROR);

	// Check Connected device
	rs2::context ctx;
	auto devices = ctx.query_devices();
	size_t device_count = devices.size();
	if (!device_count)
	{
		std::cout << "No device detected. Is it plugged in?\n";
		return EXIT_SUCCESS;
	}
	std::cout << "device detected" << std::endl;
	
	rs2::device dev = devices[0];	// Get the first connected device
	
	
	// set sensor configuration
	// void enable_stream(rs2_stream stream_type, int width, int height, rs2_format format = RS2_FORMAT_ANY, int framerate = 0)
	
	rs2::config cfg;
	cfg.enable_stream(RS2_STREAM_DEPTH,1280,720, RS2_FORMAT_Z16, 30);	// Enable default depth
	cfg.enable_stream(RS2_STREAM_COLOR,1280,720, RS2_FORMAT_RGB8,30);	// RGB8 Color Stream	
	//Problem same resolution flicker happen in aligned depth

	// load preset from json file : advance mode enabled
	if (dev.is<rs400::advanced_mode>())
	{
		auto advanced_mode_dev = dev.as<rs400::advanced_mode>();
		///////////////////////////////////////////////
		// Check if advanced-mode is enabled
		if (!advanced_mode_dev.is_enabled())
		{
			// Enable advanced-mode
			advanced_mode_dev.toggle_advanced_mode(true);			
		}
		std::cout << "Advance mode : Enabled" << std::endl;
		///////////////////////////////////////////////
		// load advance-mode preset (json file)
		std::ifstream jfile("Labv1_defaultBased_01mm_LightLowest.json");
		std::string str((std::istreambuf_iterator<char>(jfile)), std::istreambuf_iterator<char>());
		advanced_mode_dev.load_json(str);
				
	}
	else
	{
		std::cout << "Current device doesn't support advanced-mode!\n";
		return EXIT_FAILURE;
	}
	
		
	texture depth_image, color_image;			// Declare two textures on the GPU	
	// colorizer config
	rs2::colorizer color_map;					// Depth colorizer for visualize depth data
	color_map.set_option(RS2_OPTION_COLOR_SCHEME, 2); //White to black from near to far
	color_map.set_option(RS2_OPTION_HISTOGRAM_EQUALIZATION_ENABLED, 0);
	color_map.set_option(RS2_OPTION_MIN_DISTANCE, cz_minZ); // minZ
	color_map.set_option(RS2_OPTION_MAX_DISTANCE, cz_maxZ); // maxZ

	rs2::decimation_filter dec;					// Decimation filter reduces the amount of data
	dec.set_option(RS2_OPTION_FILTER_MAGNITUDE, 2);
	 
	// Define transformations from and to Disparity domain
	rs2::disparity_transform depth2disparity;
	rs2::disparity_transform disparity2depth(false);
		
	rs2::spatial_filter spat;					// Define spatial filter (edge-preserving)	
	spat.set_option(RS2_OPTION_HOLES_FILL, 2);	// Enable hole-filling, 5 = fill all the zero pixels
				  
	rs2::temporal_filter temp;					// Define temporal filter		
	rs2::align align_to(RS2_STREAM_COLOR);		// Spatially align all streams to depth viewport
	rs2::pipeline pipe;							// Declare RealSense pipeline
	auto profile = pipe.start(cfg);				// Start streaming with cfg configuration

	auto sensor = profile.get_device().first<rs2::depth_sensor>();
	auto depth_scale = sensor.get_depth_scale();

	//std::cout <<"Scale: " << depth_scale << std::endl;

	//config depth_sensor by preloaded preset	
	//sensor.set_option(rs2_option::RS2_OPTION_VISUAL_PRESET, 
	//rs2_rs400_visual_preset::RS2_RS400_VISUAL_PRESET_HIGH_ACCURACY);
	
	// Generate Rendering window : app
	auto stream = profile.get_stream(RS2_STREAM_DEPTH).as<rs2::video_stream_profile>();
	window app(stream.width(), stream.height(), "RealSense Measure Example");

	
	rs2::frame_queue postprocessed_frames;	// After post-processing, frame flow into this queue
	rs2::frame_queue nonprocessed_frames;

	std::atomic_bool alive{ true };			// alive boolean signal the threads to finish-up
	std::atomic_bool s_alive{ true };		// s_alive for waitingSave

	std::chrono::seconds timestamper;       // file namer
	timestamper = std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::system_clock::now().time_since_epoch());

	// Video-processing thread	
	std::thread video_processing_thread([&]() {
		// Video-processing thread will fetch frames from the camera,
		// apply post-processing and send the result to the main thread for rendering
		// It recieves synchronized(not spatially aligned) pairs and outputs synchronized and aligned pairs

		// main Processing block
		// In order to generate new composite frames, we have to wrap the processing
		// code in a lambda

		rs2::processing_block frame_processor( [&](rs2::frameset data, rs2::frame_source& source)
		{
			// data: Input frameset (from the pipeline)
			// source: Frame pool that can allocate new frames

			data = align_to.process(data);			//make the frames spatially aligned
														
			rs2::depth_frame depth = data.get_depth_frame();// Apply depth post-processing
			
			//depth = dec.process(depth);			// Decimation Filter (reducing resolution)						
			depth = depth2disparity.process(depth);	// switch to disparity domain			
			depth = spat.process(depth);			// Apply spatial filtering		
			depth = temp.process(depth);			// Apply temporal filtering
			depth = disparity2depth.process(depth);	// If in disparity domain, switch depth
						
			// Apply color map for visualization of depth
			auto colorized = color_map(depth).as<rs2::depth_frame>();
			auto color = data.get_color_frame();
			
			// Group the two frames together (to make sure they are rendered in sync)
			rs2::frameset combined = source.allocate_composite_frame({ colorized, color });
			
			source.frame_ready(combined);			// Send the composite frame for rendering

		});			
		frame_processor >> postprocessed_frames;	// result from block1 = push into postprocessed_frames queue	

		
		rs2::processing_block frame_processor2(	[&](rs2::frameset data, rs2::frame_source& source) 
		{

			data = align_to.process(data);			//make the frames spatially aligned

			rs2::depth_frame depth = data.get_depth_frame();// Apply depth post-processing

			//depth = dec.process(depth);			// Decimation Filter (reducing resolution)						
			depth = depth2disparity.process(depth);	// switch to disparity domain			
			depth = spat.process(depth);			// Apply spatial filtering		
			depth = temp.process(depth);			// Apply temporal filtering
			depth = disparity2depth.process(depth);	// If in disparity domain, switch depth
			
			auto color = data.get_color_frame();

			// Group the two frames together (to make sure they are rendered in sync)
			rs2::frameset combined = source.allocate_composite_frame({ depth , color });

			// Send the composite frame for rendering
			source.frame_ready(combined);

		});
		frame_processor2 >> nonprocessed_frames;	// result from block2 = push into nonprocessed_frames queue

		while (alive)
		{
			// Fetch frames from the pipeline and send them for processing
			rs2::frameset fs;
			if (pipe.poll_for_frames(&fs)) 
			{
				frame_processor.invoke(fs);
				frame_processor2.invoke(fs);
			}
		}

	});

	// Save Trigger Thread
	std::thread waitingSave([&]() {
		
		while (s_alive)
		{
			
			
			fflush(stdin);
			fflush(stdout);
			std::cout << "input key: ";
			char inputkey = getchar();
			
			
			
			if (inputkey == '1') {
				
				std::cout << "Save RGB Light-1 performed!" << std::endl;
				capture_light1(worldFrameset, rawFrameset, timestamper, inputkey);				
			}
			else if (inputkey == '2')
			{
				
				std::cout << "Save RGB Light-2 performed!" << std::endl;
				capture_light1(worldFrameset, rawFrameset, timestamper, inputkey);
			}
			else if (inputkey == 'd') 
			{
				
				std::cout << "Save Depth performed!" << std::endl;
				capture_light1(worldFrameset, rawFrameset, timestamper, inputkey);
			}
			else if( inputkey == 't')
			{
				//generate timestamper
				timestamper = std::chrono::duration_cast<std::chrono::seconds>(
					std::chrono::system_clock::now().time_since_epoch());	
				std::cout << "current timestamp : " << timestamper.count() << std::endl;
			}
			else
			{
				//std::cout << "current timestamp : " << timestamper.count() << std::endl;
				//std::cout << "current timestamp : " << timestamper.count() << std::endl;
			}
		}
	});
	
	// render windows running?
	while (app) // Application still alive?
	{
		// Fetch the latest available post-processed frameset
		static rs2::frameset current_frameset;		
		postprocessed_frames.poll_for_frame(&current_frameset);
		
		worldFrameset = current_frameset; //assign to global varriable
		
		nonprocessed_frames.poll_for_frame(&rawFrameset);

		if (current_frameset) {

			auto depth = current_frameset.get_depth_frame();
			auto color = current_frameset.get_color_frame();
			
			// Render frames			
			depth_image.render(depth, { 0,               0, app.width() / 2, app.height() });
			color_image.render(color, { app.width() / 2, 0, app.width() / 2, app.height() });
			
		}

	}

	alive = false; // 
	s_alive = false;

	video_processing_thread.join(); //parallel thread join to thread pool
	waitingSave.join();

	return EXIT_SUCCESS;
}
catch (const rs2::error & e)
{
	std::cerr << "RealSense error calling " << e.get_failed_function() << "(" << e.get_failed_args() << "):\n    " << e.what() << std::endl;
	return EXIT_FAILURE;
}
catch (const std::exception& e)
{
	std::cerr << e.what() << std::endl;
	return EXIT_FAILURE;
}

void capture_light1(const rs2::frameset& frmset, 
					const rs2::frameset& rawset, 
					const std::chrono::seconds& timstp,
					const char& workKey) 
{
	// save frame individually png file 
	// save metadata and rs_intrinsic
	auto depth_capfrm = frmset.get_depth_frame();
	auto color_capfrm = frmset.get_color_frame();
	
	std::stringstream depth_filename;
	std::stringstream color_filename1;
	std::stringstream color_filename2;
	std::stringstream rawDepth_filename;

	//std::chrono::minutes ms_time = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::system_clock::now().time_since_epoch());
	//std::cout << ms_time.count();

	depth_filename  << "output_image/"<<timstp.count() << "_depth-u8-" << depth_capfrm.get_profile().stream_name() << ".png";
	color_filename1 << "output_image/"<<timstp.count() << "_RGB-1_" << color_capfrm.get_profile().stream_name() << ".png";
	color_filename2 << "output_image/"<<timstp.count() << "_RGB-2_" << color_capfrm.get_profile().stream_name() << ".png";

	// Raw depth frame save
	auto rawDepth = rawset.get_depth_frame();
	int _height = rawDepth.get_height();
	int _width = rawDepth.get_width();

	cv::Mat depth16(cv::Size(_width, _height), CV_16UC1, (void*)rawDepth.get_data(), cv::Mat::AUTO_STEP);

	
	rawDepth_filename <<"output_image/" <<timstp.count() << "_depth-u16_" << rawDepth.get_profile().stream_name() << ".png";
	const cv::String rawfname(rawDepth_filename.str());
	

	if (workKey == '1')
	{
		stbi_write_png(
			color_filename1.str().c_str(),
			color_capfrm.get_width(),
			color_capfrm.get_height(),
			color_capfrm.get_bytes_per_pixel(),
			color_capfrm.get_data(),
			color_capfrm.get_stride_in_bytes()
		);
		std::cout << "RGB Light1 Saved : " << color_filename1.str() << std::endl;
	}
	if (workKey == '2')
	{
		stbi_write_png(
			color_filename2.str().c_str(),
			color_capfrm.get_width(),
			color_capfrm.get_height(),
			color_capfrm.get_bytes_per_pixel(),
			color_capfrm.get_data(),
			color_capfrm.get_stride_in_bytes()
		);
		std::cout << "RGB Light2 Saved : " << color_filename2.str() << std::endl;
	}
	if (workKey == 'd')
	{
		stbi_write_png(
			depth_filename.str().c_str(),
			depth_capfrm.get_width(),
			depth_capfrm.get_height(),
			depth_capfrm.get_bytes_per_pixel(),
			depth_capfrm.get_data(),
			depth_capfrm.get_stride_in_bytes()
		);
		std::cout << "Depth Colorized Saved : " << depth_filename.str() << std::endl;

		cv::imwrite(rawfname, depth16);
		std::cout << "Depth Grayscale Saved : " << rawDepth_filename.str() << std::endl;
		
	}




}




