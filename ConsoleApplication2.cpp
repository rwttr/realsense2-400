// ConsoleApplication2.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <librealsense2/rs.hpp> // Include RealSense Cross Platform API
#include "example.hpp"          // Include short list of convenience functions for rendering
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
#include <fstream>
#include <streambuf>


int main(int argc, char * argv[]) try
{
	// Check Connected device, Enable advance-mode
	rs2::context ctx;
	auto devices = ctx.query_devices();
	size_t device_count = devices.size();
	if (!device_count)
	{
		std::cout << "No device detected. Is it plugged in?\n";
		return EXIT_SUCCESS;
	}
	std::cout << "device detected" << std::endl;
	// Get the first connected device
	auto dev = devices[0];

	if (dev.is<rs400::advanced_mode>())
	{
		auto advanced_mode_dev = dev.as<rs400::advanced_mode>();
		///////////////////////////////////////////////
		// Check if advanced-mode is enabled
		if (!advanced_mode_dev.is_enabled())
		{
			// Enable advanced-mode
			advanced_mode_dev.toggle_advanced_mode(true);
			std::cout << "Advance mode : Enabled" << std::endl;
		}
		///////////////////////////////////////////////
		// load advance-mode preset (json file)
		std::ifstream jfile("demo2_preset.json");
		std::string str((std::istreambuf_iterator<char>(jfile)), std::istreambuf_iterator<char>());
		advanced_mode_dev.load_json(str);
		
		
	}
	else
	{
		std::cout << "Current device doesn't support advanced-mode!\n";
		return EXIT_FAILURE;
	}
	

	rs2::log_to_console(RS2_LOG_SEVERITY_ERROR);
		
	// Declare two textures on the GPU, one for color and one for depth
	texture depth_image, color_image;

	// Depth colorizer for visualization of depth data
	rs2::colorizer color_map;

	// Decimation filter reduces the amount of data (while preserving best samples)
	rs2::decimation_filter dec;
	dec.set_option(RS2_OPTION_FILTER_MAGNITUDE, 2);

	// Define transformations from and to Disparity domain
	rs2::disparity_transform depth2disparity;
	rs2::disparity_transform disparity2depth(false);

	// Define spatial filter (edge-preserving)
	rs2::spatial_filter spat;
	// Enable hole-filling
	spat.set_option(RS2_OPTION_HOLES_FILL, 2); // 5 = fill all the zero pixels
				  
	rs2::temporal_filter temp;				// Define temporal filter		
	rs2::align align_to(RS2_STREAM_DEPTH);	// Spatially align all streams to depth viewport
	rs2::pipeline pipe;						// Declare RealSense pipeline
	
	// Start streaming with default recommended configuration
	rs2::config cfg;
	cfg.enable_stream(RS2_STREAM_DEPTH);					// Enable default depth
	cfg.enable_stream(RS2_STREAM_COLOR, RS2_FORMAT_RGB8);	// RGB8 Color Stream	
	auto profile = pipe.start(cfg);
	
	//auto sensor = profile.get_device().first<rs2::depth_sensor>();
	//config depth_sensor by preloaded preset	
	//sensor.set_option(rs2_option::RS2_OPTION_VISUAL_PRESET, rs2_rs400_visual_preset::RS2_RS400_VISUAL_PRESET_HIGH_ACCURACY);
	
	auto stream = profile.get_stream(RS2_STREAM_DEPTH).as<rs2::video_stream_profile>();
	window app(stream.width(), stream.height(), "RealSense Measure Example");

	// After initial post-processing, frames will flow into this queue:
	rs2::frame_queue postprocessed_frames;

	// Alive boolean will signal the worker threads to finish-up
	std::atomic_bool alive{ true };

	// Video-processing thread will fetch frames from the camera,
	// apply post-processing and send the result to the main thread for rendering
	// It recieves synchronized(not spatially aligned) pairs and outputs synchronized and aligned pairs
	
	std::thread video_processing_thread([&]() {
		// In order to generate new composite frames, we have to wrap the processing
		// code in a lambda
		rs2::processing_block frame_processor(
			[&](rs2::frameset data, // Input frameset (from the pipeline)
				rs2::frame_source& source) // Frame pool that can allocate new frames
		{
			
			data = align_to.process(data);			//make the frames spatially aligned
					
			rs2::frame depth = data.get_depth_frame();// Apply depth post-processing

			//depth = dec.process(depth);			// Decimation Filter (reducing resolution)						
			depth = depth2disparity.process(depth);	// switch to disparity domain			
			depth = spat.process(depth);			// Apply spatial filtering		
			depth = temp.process(depth);			// Apply temporal filtering
			depth = disparity2depth.process(depth);	// If in disparity domain, switch depth
			
			// Apply color map for visualization of depth
			auto colorized = color_map(depth);
			auto color = data.get_color_frame();
			
			// Group the two frames together (to make sure they are rendered in sync)
			rs2::frameset combined = source.allocate_composite_frame({ colorized, color });
			
			// Send the composite frame for rendering
			source.frame_ready(combined);
		});
		
		// result from thread = push into postprocessed_frames queue
		frame_processor >> postprocessed_frames;

		while (alive)
		{
			// Fetch frames from the pipeline and send them for processing
			rs2::frameset fs;
			if (pipe.poll_for_frames(&fs)) frame_processor.invoke(fs);
		}
	});

	
	while (app) // Application still alive?
	{
		// Fetch the latest available post-processed frameset
		static rs2::frameset current_frameset;
		postprocessed_frames.poll_for_frame(&current_frameset);

		if (current_frameset) {

			auto depth = current_frameset.get_depth_frame();
			auto color = current_frameset.get_color_frame();
			
			// Render frames
			// texture.render (x, y, xwidth, yheight);
			depth_image.render(depth, { 0,               0, app.width() / 2, app.height() });
			color_image.render(color, { app.width() / 2, 0, app.width() / 2, app.height() });
		}



		//rs2::frameset data = pipe.wait_for_frames(); // Wait for next set of frames from the camera

		//rs2::frame depth = color_map(data.get_depth_frame()); // Find and colorize the depth data
		//rs2::frame color = data.get_color_frame();            // Find the color data

															  
		// Render depth on to the first half of the screen and color on to the second
		// texture.render (x, y, xwidth, yheight);
		//depth_image.render(depth, { 0,               0, app.width() / 2, app.height() });
		//color_image.render(color, { app.width() / 2, 0, app.width() / 2, app.height() });
	}
	alive = false;
	video_processing_thread.join();
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






