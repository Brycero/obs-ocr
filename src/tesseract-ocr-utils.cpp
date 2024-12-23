#include "tesseract-ocr-utils.h"
#include "plugin-support.h"
#include "obs-utils.h"
#include "consts.h"
#include "text-render-helper.h"

#include <obs-module.h>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <tesseract/baseapi.h>

#include <inja/inja.hpp>

#include <string>
#include <fstream>
#include <deque>
#include <stdexcept>
#include <algorithm>
#include <thread>

inline uint64_t get_time_ns(void)
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

void cleanup_config_files(const std::string &unique_id)
{
	check_plugin_config_folder_exists();

	// delete the user patterns file
	std::string filename = "user-patterns-" + unique_id + ".txt";
	std::string user_patterns_filepath = obs_module_config_path(filename.c_str());
	std::filesystem::remove(user_patterns_filepath.c_str());

	// delete the user patterns config file
	filename = "user-patterns" + unique_id + ".config";
	std::string patterns_config_filepath = obs_module_config_path(filename.c_str());
	std::filesystem::remove(patterns_config_filepath.c_str());

	// delete the output mask file
	filename = unique_id + ".png";
	std::string mask_filepath = obs_module_config_path(filename.c_str());
	std::filesystem::remove(mask_filepath.c_str());
}

void initialize_tesseract_ocr(filter_data *tf, bool hard_tesseract_init_required)
{
	try {
		if (hard_tesseract_init_required) {
			stop_and_join_tesseract_thread(tf);
			if (tf->tesseract_model != nullptr) {
				tf->tesseract_model->End();
				delete tf->tesseract_model;
				tf->tesseract_model = nullptr;
			}
		}

		std::lock_guard<std::mutex> lock(tf->tesseract_settings_mutex);

		char **configs = nullptr;
		int configs_size = 0;

		if (is_valid_output_source_name(tf->output_image_source_name)) {
			// make sure mask folder exists
			check_plugin_config_folder_exists();
		}

		// if the user patterns are not empty, apply them
		if (!tf->user_patterns.empty()) {
			check_plugin_config_folder_exists();
			// save the user patterns to a file in the module's config folder
			std::string filename = "user-patterns-" + tf->unique_id + ".txt";
			std::string user_patterns_filepath =
				obs_module_config_path(filename.c_str());
			obs_log(LOG_INFO, "Saving user patterns to: %s",
				user_patterns_filepath.c_str());
			std::ofstream user_patterns_file(user_patterns_filepath);
			user_patterns_file << tf->user_patterns;
			user_patterns_file.close();

			// create a .config file pointing to the patterns file
			filename = "user-patterns" + tf->unique_id + ".config";
			std::string patterns_config_filepath =
				obs_module_config_path(filename.c_str());
			obs_log(LOG_INFO, "Saving user patterns config to: %s",
				patterns_config_filepath.c_str());
			std::ofstream patterns_config_file(patterns_config_filepath);
			patterns_config_file << "user_patterns_file " << user_patterns_filepath
					     << "\n";
			patterns_config_file.close();

			// add the config file to the configs array
			configs_size = 1;
			configs = new char *[configs_size];
			configs[0] = new char[patterns_config_filepath.length() + 1];
			strcpy(configs[0], patterns_config_filepath.c_str());
		}

		if (hard_tesseract_init_required) {
			obs_log(LOG_INFO, "Loading tesseract model from: %s",
				tf->tesseractTraineddataFilepath);

			tf->tesseract_model = new tesseract::TessBaseAPI();

			// Load model
			int retval = tf->tesseract_model->Init(tf->tesseractTraineddataFilepath,
							       tf->language.c_str(),
							       tesseract::OEM_LSTM_ONLY, configs,
							       configs_size, nullptr, nullptr,
							       false);
			if (retval != 0) {
				throw std::runtime_error("Failed to initialize tesseract model");
			}
		}

		// set tesseract page segmentation mode
		tf->tesseract_model->SetPageSegMode(
			static_cast<tesseract::PageSegMode>(tf->pageSegmentationMode));

		// apply char whitlist
		tf->tesseract_model->SetVariable("tessedit_char_whitelist",
						 tf->char_whitelist.c_str());

		if (tf->enable_smoothing) {
			tf->smoothing_filter = std::make_unique<CharacterBasedSmoothingFilter>(
				tf->word_length, tf->window_size);
		}

		if (hard_tesseract_init_required) {
			// start the thread
			std::thread new_thread(tesseract_thread, tf);
			tf->tesseract_thread.swap(new_thread);
		}
	} catch (std::exception &e) {
		obs_log(LOG_ERROR, "Failed to load tesseract model: %s", e.what());
		return;
	}
}

std::string strip(const std::string &str)
{
	size_t start = str.find_first_not_of(" \t\n\r");
	size_t end = str.find_last_not_of(" \t\n\r");

	if (start == std::string::npos || end == std::string::npos)
		return "";

	return str.substr(start, end - start + 1);
}

std::string run_tesseract_ocr(filter_data *tf, const cv::Mat &image)
{
	// run the tesseract model
	tf->tesseract_model->SetImage(image.data, image.cols, image.rows, image.channels(),
				      (int)image.step);
	char *text = tf->tesseract_model->GetUTF8Text();
	if (text == nullptr) {
		return "";
	}
	std::string recognitionResult = std::string(text);
	delete[] text;

	// get the confidence of the recognition result
	const int confidence = tf->tesseract_model->MeanTextConf();

	if (confidence < tf->conf_threshold) {
		return "";
	}

	// strip whitespace from the beginning and end of the string
	recognitionResult = strip(recognitionResult);

	if (tf->enable_smoothing) {
		recognitionResult = tf->smoothing_filter->add_reading(recognitionResult);
	}

	return recognitionResult;
}

std::vector<OCRBox> extract_text_detection_boxes(filter_data *tf, cv::Size imageSize)
{
	// extract the text detection boxes
	tesseract::ResultIterator *ri = tf->tesseract_model->GetIterator();
	if (ri == nullptr) {
		return std::vector<OCRBox>();
	}
	tesseract::PageIteratorLevel level = tesseract::RIL_WORD;
	if (tf->pageSegmentationMode == tesseract::PSM_SINGLE_CHAR) {
		level = tesseract::RIL_SYMBOL;
	}
	std::vector<OCRBox> boxes;
	do {
		if (ri->Empty(level)) {
			continue;
		}
		// is this a word box?
		if (level == tesseract::RIL_WORD) {
			// get the confidence of the word
			float conf = ri->Confidence(level);
			if ((int)conf < tf->conf_threshold) {
				continue;
			}
		}
		OCRBox box;
		int left, top, right, bottom;
		ri->BoundingBox(level, &left, &top, &right, &bottom);
		box.box = cv::Rect(left, top, right - left, bottom - top);
		// get the text of the box
		const char *text = ri->GetUTF8Text(level);
		box.text = text;
		// get area of box
		const int area = (right - left) * (bottom - top);
		// if the area is too small or too big, relative to the image size - skip the box
		if (area < 100 || area > (imageSize.width * imageSize.height) / 2) {
			continue;
		}
		boxes.push_back(box);
	} while (ri->Next(level));
	delete ri;

	return boxes;
}

CharacterBasedSmoothingFilter::CharacterBasedSmoothingFilter(size_t word_length_,
							     size_t window_size_)
	: word_length(word_length_),
	  window_size(window_size_),
	  readings(word_length_, std::deque<char>(window_size_))
{
}

std::string CharacterBasedSmoothingFilter::add_reading(const std::string &inWord)
{
	std::string word = inWord;
	if (word.length() != word_length) {
		// trim the word if it's longer than the expected length
		if (word.length() > this->word_length)
			word = word.substr(0, this->word_length);
		// pad the word if it's shorter than the expected length
		if (word.length() < this->word_length)
			word = word + std::string(this->word_length - word.length(), ' ');
	}

	std::string smoothed_word;
	for (size_t i = 0; i < word_length; i++) {
		readings[i].push_back(word[i]);
		if (readings[i].size() > window_size) {
			readings[i].pop_front();
		}
		std::string window(readings[i].begin(), readings[i].end());
		// find the most common character in the window
		char most_common_char =
			*std::max_element(window.begin(), window.end(), [window](char a, char b) {
				return std::count(window.begin(), window.end(), a) <
				       std::count(window.begin(), window.end(), b);
			});
		smoothed_word += most_common_char;
	}

	return smoothed_word;
}

std::string format_text_with_template(inja::Environment &env, const std::string &text,
				      struct filter_data *tf)
{
	// Replace the {{output}} placeholder with the source text using inja
	nlohmann::json data;
	data["output"] = text;
	return env.render(tf->output_format_template, data);
}

void stop_and_join_tesseract_thread(struct filter_data *tf)
{
	{
		std::lock_guard<std::mutex> lock(tf->tesseract_mutex);
		if (!tf->tesseract_thread_run) {
			// Thread is already stopped
			return;
		}
		tf->tesseract_thread_run = false;
	}
	tf->tesseract_thread_cv.notify_all();
	if (tf->tesseract_thread.joinable()) {
		tf->tesseract_thread.join();
	}
}

// Tesseract thread function
void tesseract_thread(void *data)
{
	filter_data *tf = reinterpret_cast<filter_data *>(data);

	{
		std::lock_guard<std::mutex> lock(tf->tesseract_mutex);
		tf->tesseract_thread_run = true;
	}

	obs_log(LOG_INFO, "Starting Tesseract thread, update timer: %d", tf->update_timer_ms);

	inja::Environment env;

	while (true) {
		{
			std::lock_guard<std::mutex> lock(tf->tesseract_mutex);
			if (!tf->tesseract_thread_run) {
				break;
			}
		}

		// time the operation
		uint64_t request_start_time_ns = get_time_ns();

		// Send the image to the Tesseract OCR model
		cv::Mat imageBGRA;
		{
			std::unique_lock<std::mutex> lock(tf->inputBGRALock, std::try_to_lock);
			if (lock.owns_lock()) {
				imageBGRA = tf->inputBGRA.clone();
			}
		}

		if (!imageBGRA.empty()) {
			try {
				std::lock_guard<std::mutex> lock(tf->tesseract_settings_mutex);

				// if update on change is true check if the image has changed
				if (tf->update_on_change &&
				    imageBGRA.size() == tf->lastInputBGRA.size()) {
					const int change_threshold_from_image_area =
						(int)((float)tf->update_on_change_threshold /
						      100.0f *
						      (float)(imageBGRA.cols * imageBGRA.rows));
					// if the image has not changed, skip the processing
					// take the absolute difference between the images, convert to gray and count the non-zero pixels
					cv::Mat diff;
					cv::absdiff(imageBGRA, tf->lastInputBGRA, diff);
					cv::cvtColor(diff, diff, cv::COLOR_BGRA2GRAY);
					if (cv::countNonZero(diff) <
					    change_threshold_from_image_area) {
						// skip the processing
						continue;
					}
				}
				tf->lastInputBGRA = imageBGRA.clone();

				cv::Mat imageForOCR = imageBGRA.clone();

				// if threshold is requested, apply it
				if (tf->binarizationMode != 0) {
					cv::Mat gray;
					cv::cvtColor(imageForOCR, gray, cv::COLOR_BGRA2GRAY);

					if (tf->binarizationMode == 1)
						cv::threshold(gray, imageForOCR,
							      tf->binarizationThreshold, 255,
							      cv::THRESH_BINARY);
					else if (tf->binarizationMode == 2 ||
						 tf->binarizationMode == 3) {
						// ensure that the block size is odd
						int block_size = tf->binarizationBlockSize;
						if (tf->binarizationBlockSize % 2 == 0) {
							block_size++;
						}
						if (tf->binarizationMode == 2) {
							cv::adaptiveThreshold(
								gray, imageForOCR, 255,
								cv::ADAPTIVE_THRESH_MEAN_C,
								cv::THRESH_BINARY, block_size, 2);
						} else {
							cv::adaptiveThreshold(
								gray, imageForOCR, 255,
								cv::ADAPTIVE_THRESH_GAUSSIAN_C,
								cv::THRESH_BINARY, block_size, 2);
						}
					} else if (tf->binarizationMode == 4)
						cv::threshold(gray, imageForOCR, 0, 255,
							      cv::THRESH_BINARY |
								      cv::THRESH_TRIANGLE);
					else if (tf->binarizationMode == 5)
						cv::threshold(gray, imageForOCR, 0, 255,
							      cv::THRESH_BINARY | cv::THRESH_OTSU);
				}

				if (tf->dilationIterations > 0) {
					cv::Mat element = cv::getStructuringElement(cv::MORPH_RECT,
										    cv::Size(3, 3));
					cv::dilate(imageForOCR, imageForOCR, element,
						   cv::Point(-1, -1), tf->dilationIterations);
				}

				if (tf->previewBinarization) {
					// lock the outputPreviewBGRALock
					std::lock_guard<std::mutex> lock(tf->outputPreviewBGRALock);
					if (imageForOCR.channels() == 4) {
						imageForOCR.copyTo(tf->outputPreviewBGRA);
					} else {
						cv::cvtColor(imageForOCR, tf->outputPreviewBGRA,
							     cv::COLOR_GRAY2BGRA);
					}
				}

				if (tf->rescaleImage) {
					// scale to height tf->rescaleTargetSize maintaining aspect ratio
					cv::Mat resized;
					float scale = (float)tf->rescaleTargetSize /
						      (float)imageForOCR.rows;
					cv::resize(imageForOCR, resized, cv::Size(), scale, scale);
					imageForOCR = resized;
				}

				// Process the image
				std::string ocr_result = run_tesseract_ocr(tf, imageForOCR);

				if (is_valid_output_source_name(tf->output_image_source_name)) {
					cv::Mat text_detection_output(imageBGRA.rows,
								      imageBGRA.cols, CV_8UC4,
								      cv::Scalar(0, 0, 0, 0));

					// Extract the text detection boxes
					std::vector<OCRBox> boxes =
						extract_text_detection_boxes(tf, imageBGRA.size());

					if (tf->output_image_option ==
					    OUTPUT_IMAGE_OPTION_DETECTION_MASK) {
						text_detection_output.setTo(
							cv::Scalar(0, 0, 0, 255));

						// Create a text detection binary mask
						for (const auto &box : boxes) {
							cv::rectangle(
								text_detection_output, box.box,
								cv::Scalar(255, 255, 255, 255), -1);
						}
					} else {
						// Create a text overlay image
						QImage text_overlay_image = render_boxes_with_qtextdocument(
							boxes, imageBGRA.cols, imageBGRA.rows,
							tf->output_image_option ==
								OUTPUT_IMAGE_OPTION_TEXT_BACKGROUND);
						cv::Mat text_overlay_image_mat(
							text_overlay_image.height(),
							text_overlay_image.width(), CV_8UC4,
							text_overlay_image.bits(),
							text_overlay_image.bytesPerLine());
						text_overlay_image_mat.copyTo(
							text_detection_output);
					}

					setTextDetectionMaskCallback(text_detection_output, tf);
				}

				if (!ocr_result.empty() &&
				    is_valid_output_source_name(tf->output_source_name)) {
					// If an output source is selected - send the results there
					ocr_result = format_text_with_template(env, ocr_result, tf);
					setTextCallback(ocr_result, tf);
				}
			} catch (const std::exception &e) {
				obs_log(LOG_ERROR, "%s", e.what());
			}
		}

		// time the request, calculate the remaining time and sleep
		const uint64_t request_end_time_ns = get_time_ns();
		const uint64_t request_time_ns = request_end_time_ns - request_start_time_ns;
		const int64_t sleep_time_ms =
			(int64_t)(tf->update_timer_ms) - (int64_t)(request_time_ns / 1000000);
		if (sleep_time_ms > 0) {
			std::unique_lock<std::mutex> lock(tf->tesseract_mutex);
			// Sleep for n ns as per the update timer for the remaining time
			tf->tesseract_thread_cv.wait_for(lock,
							 std::chrono::milliseconds(sleep_time_ms));
		}
	}
	obs_log(LOG_INFO, "Stopping Tesseract thread");

	{
		std::lock_guard<std::mutex> lock(tf->tesseract_mutex);
		tf->tesseract_thread_run = false;
	}
}
