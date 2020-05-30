#pragma once

#include <optional>
#include <filesystem>
#include <fstream>
#include <vector>

namespace AlphaFile {
	using Natural = size_t;
	using Absolute = size_t;

	struct OpenFlags {
		bool write = true;

		explicit OpenFlags (bool t_write=true) : write(t_write) {}

		std::ios_base::openmode getOpenmode () const {
			std::ios_base::openmode openmode = std::ios_base::in & std::ios_base::binary;

			if (write) {
				openmode = openmode & std::ios_base::out;
			}

			return openmode;
		}
	};
	
	// ===== Errors =====
	// ==== Open Errors ====
	struct OpenError : public std::runtime_error {
		std::filesystem::path filename;
		OpenError (std::filesystem::path t_filename, std::string message) : std::runtime_error(message), filename(t_filename) {}
	};
	struct FileDoesNotExist : public OpenError {
		FileDoesNotExist (std::filesystem::path t_filename) : OpenError(t_filename, "File does not exist") {}
	};
	struct UnopenableFile : public OpenError {
		UnopenableFile (std::filesystem::path t_filename, std::string message) : OpenError(t_filename, "File could not be opened: " + message) {}
	};
	struct UnknownOpenError : public OpenError {
		UnknownOpenError (std::filesystem::path t_filename, std::string message) : OpenError(t_filename, "Unknown error in opening file: " + message) {}
	};

	// ==== Read Errors ====
	struct ReadError : public std::runtime_error {
		size_t position;
		size_t amount;
		ReadError (size_t t_position, size_t amount, std::string message) : std::runtime_error(message), position(t_position) {}
	};

	namespace detail {
		/// Seeks to position, then reads into [data] of at most [amount] bytes
		/// May have errors, which should be checked by caller
		/// Returns the amount of bytes read (assuming no errors)
		static size_t simpleRead (std::fstream& file, size_t position, std::byte* data, size_t amount) {
			file.seekg(position);
			file.read(reinterpret_cast<char*>(data), amount);
			return file.gcount();
		}

		static void simpleWrite (std::fstream& file, size_t position, const std::byte* data, size_t amount) {
			file.seekp(position);
			file.write(reinterpret_cast<const char*>(data), amount);
		}
	}

	class BasicFile {
		protected:

		std::fstream file;
		std::filesystem::path filename;
		OpenFlags open_flags;

		public:

		explicit BasicFile () {}

		std::filesystem::path getFilename () const {
			return filename;
		}

		/// Opens the specified file
		/// If this throws, then this is in an undefined state
		///  TODO: which can be remedied by calling open once more (and succeeding)
		void open (OpenFlags t_open_flags, std::filesystem::path t_filename) {
			close();

			if (!std::filesystem::exists(t_filename)) {
				throw FileDoesNotExist(t_filename);
			}

			if (std::filesystem::is_directory(t_filename)) {
				throw UnopenableFile(t_filename, "Cannot open directory");
			}

			if (std::filesystem::is_character_file(t_filename)) {
				throw UnopenableFile(t_filename, "Cannot open special character file");
			}

			if (std::filesystem::is_fifo(t_filename)) {
				throw UnopenableFile(t_filename, "Cannot open fifo");
			}

			if (std::filesystem::is_socket(t_filename)) {
				throw UnopenableFile(t_filename, "Cannot open socket");
			}

			filename = t_filename;
			open_flags = t_open_flags;
			file.open(filename, open_flags.getOpenmode());

			if (file.fail()) {
				// TODO: find if there's a way to get any error information
				throw UnknownOpenError(filename, "unknown reason");
			}
		}

		void close () {
			if (isOpen()) {
				file.close();
			}
		}

		void reopen () {
			open(open_flags, filename);
		}

		bool isOpen () const {
			return file.is_open();
		}

		bool isWritable () const {
			return open_flags.write;
		}

		std::optional<std::byte> read (size_t position) {
			std::byte value{0x00};
			const size_t amount_read  = read(position, &value, 1);

			if (amount_read == 0) {
				return std::nullopt;
			}
			return value;
		}

		std::vector<std::byte> read (size_t position, size_t amount) {
			std::vector<std::byte> values;
			// Resize to [amount], this will default construct (aka, null bytes) that many entries
			// we have to resize (rather than reserve) since we're writing into them
			// (We could still technically resize instead, but that sounds more iffy)
			values.resize(amount);

			const size_t amount_read = read(position, values.data(), amount);

			// resize down to the actual amount we read
			// since anything past that will simply be incorrect values
			values.resize(amount_read);
			// tell the vector to shrink it's allocation down to the new size, since it likely won't change further
			values.shrink_to_fit();

			return values;
		}

		size_t read (size_t position, std::byte* data, size_t amount) {
			return internalRead(position, data, amount);
		}

		void edit (size_t position, std::byte value) {
			edit(position, &value, 1);
		}

		void edit (size_t position, const std::vector<std::byte>& values) {
			edit(position, values.data(), values.size());
		}

		template<size_t N>
		void edit (size_t position, const std::array<std::byte, N>& values) {
			edit(position, values.data(), values.size());
		}

		void edit (size_t position, const std::byte* values, size_t amount) {
			detail::simpleWrite(file, position, values, amount);
		}

		size_t getSize () {
			close();
			const size_t length = std::filesystem::file_size(filename);
			reopen();

			return length;
		}

		void resize (size_t amount) {
			close();
			std::filesystem::resize_file(filename, amount);
			reopen();
		}

		void insertionNoOverwrite (size_t absolute_position, size_t amount, size_t chunk_size) {
            // This function only inserts null bytes. Replacing them is an action which can be done afterwards
            // TODO: this could potentially be done in this function if we were given a vector of bytes, and could be a separate function pretty easily.

            // Imagine (each character being an individual byte):
            //  data               = {a b c d e f g h i j k l m n o p q r s t u v w x y z}
            // We insert one byte (0) at position 0 (position is zero-indexed) into data
            //  insert(data, 0, 1) = {0 a b c d e f g h i j k l m n o p q r s t u v w x y z}
            // If we were to insert one byte (0) at position 5 into data
            //  insert(data, 5, 1) = {a b c d e 0 f g h i j k l m n o p q r s t u v w x y z}
            // Simple, but should explain how we get there.
            // Let's use [chunk_size] as the size of the chunks we should make.
            //  We'll assume [chunk_size] is 4.
            // insert(data, 5, 1) would be done as:
            //  {a b c d e f g h i j k l m n o p q r s t u v w x y z}
            //            ^ We insert here, since that is where position five is. This would be before the 'f' byte.
            // Divide it into chunks of [chunk_size], but only the parts *after* where we are inserting. (Thus, everything before the insertion point is untouched)
            //   There may be 0 chunks, and the last chunk does not have to be equivalent. All of the chunk's sizes are <= chunk_size
            //  {a b c d e {f g h i} {j k l m} {n o p q} {r s t u} {v w x y} {z}}
            //            ^
            // We then need to add new bytes to the end of the file, since we are *expanding* it.
            // We wouold need to add [amount] bytes to the end of the file, their value does not matter.
            //  {a b c d e {f g h i} {j k l m} {n o p q} {r s t u} {v w x y} {z} 0}
            // Since we want to shift everything forward/rightward, we need to start at the end and work our way backwards.
            //   (If you're confused, then realize that if we attempted to shift {f g h i} forward, it would overwrite part of the chunk directly after it.
            //    We could keep track of overwritten data, but I believe it's easier to simply do it in reverse order. That way there's less temp data.)
            //  So we start with {z}, and we move it forward by [amount] bytes.
            //  {a b c d e {f g h i} {j k l m} {n o p q} {r s t u} {v w x y} z | z}
            // You will have noticed that it has 'duplicated', since we don't bother overwriting the *old* {z}.
            //  z is no longer in a chunk like before since it's now just a value to overwrite. The symbol | is for the actual file data that we won't touch again.
            // Then we shift {v w x y} forward by [amount] (1)
            //  {a b c d e {f g h i} {j k l m} {n o p q} {r s t u} v | v w x y z}
            // As you can see, we keep the old start of it, since we don't bother overwriting what we left behind, and then we continue this pattern.
            // Following, we get:
            //  {a b c d e {f g h i} j | j k l m n o p q r s t u v w x y z}
            // Then we do the last chunk of {f g h i }
            //  {a b c d e f | f g h i j k l m n o p q r s t u v w x y z}
            //            ^ our insertion point is now simply an 'overwrite' point, since we now have an extra byte in that position.
            // So we simply edit that byte with the value (0)
            //  {a b c d e 0 f g h i j k l m n o p q r s t u v w x y z} after
            //  {a b c d e f g h i j k l m n o p q r s t u v w x y z} before
            //


            // We start shifting data at the end of the file.
            // we call it end to avoid confusion, even if it's technically the start.
            const size_t shift_end = getSize();

            // The amount of bytes we have to move.
            const size_t shift_amount = shift_end - absolute_position;
            // The amount of chunks we have (and thus the amount of shifts we must make).
            const size_t shift_iterations = (shift_amount / chunk_size) + (shift_amount % chunk_size == 0 ? 0 : 1);

            // The amount in the first slice (if one exists). Either chunk_size (file is divided into equal chunks) or the remainder.
            const size_t first_slice_amount = (shift_end % chunk_size) == 0 ? chunk_size : (shift_end % chunk_size);

            // statically sized as [chunk_size]
            std::vector<std::byte> transpose_data;
            transpose_data.resize(chunk_size);
            for (size_t i = 0; i < shift_iterations; i++) {
                // First iteration is on the last chunk, so then it could be a size <= chunk_size, while all other chunks are == chunk_size
                const size_t slice_start = shift_end - first_slice_amount - (i * chunk_size);
                const size_t slice_amount = i == 0 ? first_slice_amount : chunk_size;
                const size_t slice_destination = slice_start + amount;

                const size_t slice_read_amount = read(slice_start, transpose_data.data(), slice_amount);
                if (slice_read_amount != slice_amount) {
                    // TODO: should this be a hard error?
                    //std::cout << "[insertion] slice_read_amount (" << slice_read_amount << ") is not equivalent to slice_amount (" << slice_amount << ")\n";
                }

                edit(slice_destination, transpose_data.data(), slice_read_amount);
            }
        }

		/// Insert's bytes into the file.
        /// Technically resizes the file since it writes more bytes out than was originally in the file
        /// So .resize is not needed (though can be done without harm)
        void insertion (size_t absolute_position, size_t amount, size_t chunk_size) {
            insertionNoOverwrite(absolute_position, amount, chunk_size);

            // Writes out the series in chunk_size chunks.

            // An array of 0's to write.
            std::vector<std::byte> data;
            data.resize(chunk_size);
            for (size_t i = 0; i < data.size(); i++) {
                data[i] = std::byte(0x00);
            }

            const size_t amount_end = absolute_position + amount;
            const size_t amount_iterations = (amount / chunk_size) + (amount % chunk_size == 0 ? 0 : 1);
            for (size_t i = 0; i < amount_iterations; i++) {
                const size_t slice_start = absolute_position + (i * chunk_size);
                const size_t slice_end = std::min(slice_start + chunk_size, amount_end);
                const size_t slice_amount = slice_end - slice_start;

                edit(slice_start, data.data(), slice_amount);
            }
        }

		/// Insert's bytes into the file.
        /// .resize is not needed.
        /// [data] is the data which should be inserted into the file at [absolute_position]
        void insertion (size_t absolute_position, const std::vector<std::byte>& data, size_t chunk_size) {
            insertionNoOverwrite(absolute_position, data.size(), chunk_size);
			// TODO: is this correct?
            edit(absolute_position, data);
        }

        /// Delete's bytes from the file
        /// Note that it does NOT resize the file. It is up to the caller to call .resize with the appropriate size.
        void deletion (size_t absolute_position, size_t amount, size_t chunk_size) {
            // We only want to shift what's after the deletion.
            const size_t shift_start = absolute_position + amount;
            // Obviously we want to stop at the end of the file.
            const size_t shift_end = getSize();
            // How many bytes will be shifted over.
            const size_t shift_amount = shift_end - shift_start;
            // The amount of shifts we'll have to do.
            const size_t shift_iterations = (shift_amount / chunk_size) + (shift_amount % chunk_size == 0 ? 0 : 1);
            // Used to store the data that we are moving.
            std::vector<std::byte> transpose_data;
            transpose_data.resize(chunk_size);
            for (size_t i = 0; i < shift_iterations; i++) {
                // Where we start reading from, the data we're going to move
                const size_t slice_start = shift_start + (i * chunk_size);
                // Where we're gonna end at. Either after [chunk_size] bytes or at the end of the data we're shifting (aka end of file)
                const size_t slice_end = std::min(slice_start + chunk_size, shift_end);
                // The amount of bytes between the end and the start
                const size_t slice_amount = slice_end - slice_start;

                // Read the data into a variable, and also get how many bytes were read
                const size_t slice_read_amount = read(slice_start, transpose_data.data(), slice_amount);
                // Check if the bytes we read were equivalent to the amount given
                if (slice_read_amount != slice_amount) {
                    // TODO: should this be a hard error?
                    //std::cout << "[deletion] slice_read_amount (" << slice_read_amount << ") is not equivalent to slice_amount (" << slice_amount << ")\n";
                }
                // Write the data back, shifted forward?
                edit(slice_start - amount, transpose_data.data(), slice_read_amount);
            }

            // TODO: return the amount we deleted?
        }

		protected:

		void clearErrorState () {
			file.clear();
		}

		/// Reads [amount] bytes at pos [position] into [data]
		/// position - absolute position to read from in the file
		/// data - pointer to data to be written into [non-null]
		/// amount - max amount of bytes to write into [data]
		/// return - the amount of bytes that were read
		size_t internalRead (size_t position, std::byte* data, size_t amount) {
			const size_t amount_read = detail::simpleRead(file, position, data, amount);

			if (file.fail() && !file.eof()) {
				// TODO: look to see if there's more info on this error state
				throw ReadError(position, amount, "unknown reason");
			}

			// If (file.fail() && file.eof()) -> we hit the end of the file
			// which is fine, but we still need to clear the error state to continue using the function
			clearErrorState();

			// As far as I know, this should never happen
			if (amount_read > amount) {
				throw ReadError(position, amount, "Amount of bytes read from file exceeded amount requested, this is alarming");
			}

			return amount_read;
		}
	};

	/// A file that is constrained to only a portion of the actual file
	/// all positions are 'Natural', and do not take into account the offset
	/// ex: read(42, 1000) reads 1000 bytes from (start + 42)
	class ConstrainedFile {
		protected:
		BasicFile file;

		std::optional<Absolute> start;
		std::optional<Absolute> end;
		public:

		explicit ConstrainedFile () {}

		void setStart (std::optional<Absolute> t_start) {
			start = t_start;
		}

		void setEnd (std::optional<Absolute> t_end) {
			end = t_end;
		}

		Absolute convert (Natural position) const {
			return position + start.value_or(0);
		}

		std::filesystem::path getFilename () const {
			return file.getFilename();
		}

		void open (OpenFlags t_open_flags, std::filesystem::path t_filename) {
			return file.open(t_open_flags, t_filename);
		}

		void close () {
			return file.close();
		}

		void reopen () {
			return file.reopen();
		}

		bool isWritable () const {
			return file.isWritable();
		}

		std::optional<std::byte> read (Natural position) {
			return file.read(convert(position));
		}

		std::vector<std::byte> read (Natural position, size_t amount) {
			return file.read(convert(position), amount);
		}

		size_t read (Natural position, std::byte* data, size_t amount) {
			return file.read(convert(position), data, amount);
		}

		void edit (Natural position, std::byte value) {
			return edit(convert(position), value);
		}

		void edit (Natural position, const std::vector<std::byte>& values) {
			return edit(convert(position), values);
		}

		template<size_t N>
		void edit (Natural position, const std::array<std::byte, N>& values) {
			return edit(convert(position), values);
		}

		void edit (Natural position, const std::byte* values, size_t amount) {
			return edit(convert(position), values, amount);
		}

		/// Returns the entire file size, NOT the size of the constrained area
		size_t getSize () {
			return file.getSize();
		}

		/// Returns the constrained size
		size_t getEditableSize () {
			const size_t size = getSize();
			return end.value_or(size) - start.value_or(0);
		}

		void resize (size_t amount) {
			return file.resize(amount);
		}

		void insertionNoOverwrite (Natural position, size_t amount, size_t chunk_size) {
			return file.insertionNoOverwrite(convert(position), amount, chunk_size);
		}

		void insertion (Natural position, size_t amount, size_t chunk_size) {
			return file.insertion(convert(position), amount, chunk_size);
		}

		void insertion (Natural position, const std::vector<std::byte>& data, size_t chunk_size) {
			return file.insertion(convert(position), data, chunk_size);
		}

		/// Delete's bytes from the file
		/// Note that it does not resize the file. It is upto the caller to call .resize with the appropriate size
		void deletion (Natural position, size_t amount, size_t chunk_size) {
			return file.deletion(convert(position), amount, chunk_size);
		}
	};
}