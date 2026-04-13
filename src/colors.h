#pragma once

#define CP_SELECTED 2 ///< Color pair for selected file
#define CP_SELECTED_MULTI 3 ///< Color pair for selected file that is also multi-selected
#define CP_INDEX 4 ///< Color pair for file index (the number on the left of the file name)
#define CP_PROMPT 5 ///< Color pair for prompts (like the "GOTO: " prompt when pressing 'g')
#define CP_TITLE_UF 6 ///< Color pair for unfocused titles (like the "PREVIEW" title when the preview is not focused)
#define CP_TITLE_F 7 ///< Color pair for focused titles (like the "PREVIEW" title when the preview is focused)
#define CP_DIR_FULL 8 ///< Color pair for non-empty directories
#define CP_DIR_EMPTY 9 ///< Color pair for empty directories
#define CP_FILE 10 ///< Color pair for regular files (not categorized by extension)

#define CP_EXT_IMAGE 11 ///< Color pair for image files (like .jpg, .png, etc.)
#define CP_EXT_VIDEO 12 ///< Color pair for video files (like .mp4, .mkv, etc.)
#define CP_EXT_AUDIO 13 ///< Color pair for audio files (like .mp3, .wav, etc.)
#define CP_EXT_ARCHIVE 14 ///< Color pair for archive files (like .zip, .tar, etc.)
#define CP_EXT_CODE 15 ///< Color pair for code files (like .c, .cpp, .py, etc.)
#define CP_EXT_DOC 16 ///< Color pair for document files (like .txt, .pdf, .docx, etc.)
#define CP_EXT_EXEC 17 ///< Color pair for Binary executable files (like .exe, or files with executable permissions on Unix-like systems)
#define CP_EXT_DATA 18 ///< Color pair for data files (like .csv, .json, etc.)

#define CP_SEL_IMAGE 21 ///< Color pair for selected image files
#define CP_SEL_VIDEO 22 ///< Color pair for selected video files
#define CP_SEL_AUDIO 23 ///< Color pair for selected audio files
#define CP_SEL_ARCHIVE 24 ///< Color pair for selected archive files
#define CP_SEL_CODE 25 ///< Color pair for selected code files
#define CP_SEL_DOC 26 ///< Color pair for selected document files
#define CP_SEL_EXEC 27 ///< Color pair for selected executable files
#define CP_SEL_DATA 28 ///< Color pair for selected data files
#define CP_SEL_DIR 29 ///< Color pair for selected directories
#define CP_SEL_FILE 30 ///< Color pair for selected regular files (not categorized by extension)

#define CP_MULTI_SEL 31 ///< Color pair for files that are multi-selected (selected as part of a selection range, but not the primary selected file)
#define CP_DELETE_CONFIRM 32 ///< Color pair for delete confirmation prompts (like when pressing 'd' to delete a file)
#define CP_TRASH_CONFIRM 33 ///< Color pair for trash confirmation prompts (like when pressing 't' to move a file to trash)
